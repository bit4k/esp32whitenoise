Import("env")
import os
import shutil

def copy_binaries(source, target, env):
    env_name = env.get("PIOENV")
    
    bin_dir = os.path.join(env.get("PROJECT_DIR"), "bin")
    if not os.path.exists(bin_dir):
        os.makedirs(bin_dir)
        
    firmware_src = os.path.join(env.get("PROJECT_BUILD_DIR"), env_name, "firmware.bin")
    firmware_dst = os.path.join(bin_dir, f"{env_name}_firmware.bin")
    
    if os.path.exists(firmware_src):
        shutil.copy(firmware_src, firmware_dst)
        print(f"============================================")
        print(f"Copied firmware to: bin/{env_name}_firmware.bin")
        print(f"============================================")

env.AddPostAction("buildprog", copy_binaries)
