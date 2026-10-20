"""PlatformIO post-build check: CSV layout and actual firmware size."""
Import("env")
import subprocess


def check_image(source, target, env):
    subprocess.run(
        [env.subst("$PYTHONEXE"), env.subst("$PROJECT_DIR/scripts/check_partitions.py"),
         "--table", env.subst("$PROJECT_DIR/") + env.BoardConfig().get("build.partitions"),
         "--firmware", str(target[0])], check=True,
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_image)
