import asyncio
import os
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional


class SandboxedExecutor:
    """
    Python client interface for the rootless C Fork-Server sandbox.
    Handles workspace creation, background compilation, and execution over Unix sockets.
    """

    def __init__(self, socket_path: str = "/tmp/sandbox.sock", workspaces_root: str = "/tmp/workspaces"):
        self.socket_path = socket_path
        self.workspaces_root = Path(workspaces_root)
        self.workspaces_root.mkdir(parents=True, exist_ok=True)

    async def _compile(self, language: str, workspace_dir: Path) -> Optional[dict]:
        """
        Compiles C, C++, or Java source files in the background using asyncio.create_subprocess_shell.
        Returns None if compilation succeeds, or an error dict if compilation fails.
        """
        lang = language.lower().strip()
        if lang == "c":
            cmd = "gcc -O2 main.c -o a.out"
        elif lang in ("cpp", "c++", "cxx"):
            cmd = "g++ -O2 main.cpp -o a.out"
        elif lang == "java":
            cmd = "javac --enable-preview --source 27 Main.java || javac Main.java"
        else:
            return None

        proc = await asyncio.create_subprocess_shell(
            cmd,
            cwd=str(workspace_dir),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await proc.communicate()

        if proc.returncode != 0:
            err_msg = stderr.decode("utf-8", errors="replace").strip()
            if not err_msg:
                err_msg = stdout.decode("utf-8", errors="replace").strip()
            return {
                "status": "error",
                "error": err_msg or "Compilation failed",
                "output": err_msg or "Compilation failed",
            }
        return None

    async def _execute_socket(self, command_path: str, args: list, input_data: str) -> dict:
        """
        Sends the execution command and standard input payload over the Unix domain socket.
        """
        writer = None
        try:
            reader, writer = await asyncio.open_unix_connection(self.socket_path)

            # Construct protocol string: command|arg1|arg2\n[Standard Input Data]
            tokens = [command_path] + list(args if args is not None else [])
            cmd_header = "|".join(tokens)
            payload = f"{cmd_header}\n{input_data}"

            writer.write(payload.encode("utf-8"))
            await writer.drain()

            if writer.can_write_eof():
                writer.write_eof()

            raw_output = await reader.read()
            result = raw_output.decode("utf-8", errors="replace")

            return {
                "status": "success",
                "output": result,
            }
        except Exception as e:
            return {
                "status": "error",
                "error": str(e),
                "output": str(e),
            }
        finally:
            if writer is not None:
                try:
                    writer.close()
                    await writer.wait_closed()
                except Exception:
                    pass

    async def execute(self, command_or_language: str, args_or_code: Any = None, input_data: str = "") -> dict:
        """
        Backwards-compatible execution entrypoint supporting both raw socket commands and language code runs.
        """
        if isinstance(args_or_code, str) and command_or_language.lower().strip() in (
            "c", "cpp", "c++", "cxx", "java", "python", "python3", "py"
        ):
            return await self.run_code(command_or_language, args_or_code, input_data)

        args = args_or_code if isinstance(args_or_code, list) else []
        return await self._execute_socket(command_or_language, args, input_data)

    async def run_code(self, language: str, source_code: str, input_data: str = "") -> dict:
        """
        Sets up unique workspace, compiles if needed, and executes untrusted code via the C daemon.

        :param language: Language identifier ('c', 'cpp', 'java', 'python').
        :param source_code: Raw source code string.
        :param input_data: Standard input stream to pass to the running process.
        :return: Dict containing status and output or error information.
        """
        lang = language.lower().strip()
        run_id = str(uuid.uuid4())
        workspace_dir = self.workspaces_root / run_id
        workspace_dir.mkdir(parents=True, exist_ok=True)

        try:
            # 1. Determine file name, write source code, and prepare execution command
            if lang in ("python", "python3", "py"):
                source_file = workspace_dir / "script.py"
                source_file.write_text(source_code, encoding="utf-8")
                exec_cmd = ["/usr/bin/python3", str(source_file)]

            elif lang == "c":
                source_file = workspace_dir / "main.c"
                source_file.write_text(source_code, encoding="utf-8")
                compile_err = await self._compile(lang, workspace_dir)
                if compile_err:
                    return compile_err
                exec_cmd = [str(workspace_dir / "a.out")]

            elif lang in ("cpp", "c++", "cxx"):
                source_file = workspace_dir / "main.cpp"
                source_file.write_text(source_code, encoding="utf-8")
                compile_err = await self._compile(lang, workspace_dir)
                if compile_err:
                    return compile_err
                exec_cmd = [str(workspace_dir / "a.out")]

            elif lang == "java":
                source_file = workspace_dir / "Main.java"
                source_file.write_text(source_code, encoding="utf-8")
                compile_err = await self._compile(lang, workspace_dir)
                if compile_err:
                    return compile_err
                exec_cmd = ["/usr/bin/java", "-Xmx256m", "-Xms32m", "--enable-preview", "-cp", str(workspace_dir), "Main"]

            else:
                return {
                    "status": "error",
                    "error": f"Unsupported language: {language}",
                    "output": f"Unsupported language: {language}",
                }

            # 2. Dispatch to Unix Domain Socket
            return await self._execute_socket(exec_cmd[0], exec_cmd[1:], input_data)

        except Exception as e:
            return {
                "status": "error",
                "error": str(e),
                "output": str(e),
            }


if __name__ == "__main__":
    async def main():
        executor = SandboxedExecutor()
        c_code = """
        #include <stdio.h>
        int main() {
            printf("Hello from C!\\n");
            return 0;
        }
        """
        print("Testing run_code with C...")
        res = await executor.run_code("c", c_code)
        print("Result:", res)

    asyncio.run(main())
