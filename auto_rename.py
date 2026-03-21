Import("env")
import os
import re
import json
import shutil
import subprocess

def after_build(source, target, env):
    print("\n\033[92m=== AUTO RENAME, JSON & GITHUB PUSH ===\033[0m")
    
    # 1. Đọc Config.h để lấy Version và Tên bản cập nhật
    config_path = os.path.join(env.get("PROJECT_DIR"), "include", "Config.h")
    version = "v1.0"
    fw_name = "Update"

    if os.path.exists(config_path):
        with open(config_path, "r", encoding="utf-8") as f:
            content = f.read()
            v_match = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', content)
            n_match = re.search(r'#define\s+FIRMWARE_NAME\s+"([^"]+)"', content)
            if v_match: version = v_match.group(1)
            if n_match: fw_name = n_match.group(1)

    # 2. Copy và đổi tên file .bin ra thư mục gốc
    bin_path = str(target[0])
    new_bin_name = f"firmware_{version}.bin"
    new_bin_path = os.path.join(env.get("PROJECT_DIR"), new_bin_name)

    shutil.copy(bin_path, new_bin_path)
    print(f"[*] Da tao file firmware: {new_bin_name}")

    # 3. Cập nhật thẳng vào file versions.json
    json_path = os.path.join(env.get("PROJECT_DIR"), "versions.json")
    github_url = f"https://raw.githubusercontent.com/ngocvu124/Remote-Lamp-OTA/main/{new_bin_name}"

    data = []
    if os.path.exists(json_path):
        with open(json_path, "r", encoding="utf-8") as f:
            try:
                data = json.load(f)
            except:
                pass

    # Xóa bản cũ nếu trùng version, chèn bản mới lên đầu danh sách
    data = [entry for entry in data if entry.get("url") != github_url]
    data.insert(0, {
        "name": f"{version} ({fw_name})",
        "url": github_url
    })

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

    print("[*] Da cap nhat file versions.json")

    # 4. TỰ ĐỘNG ĐẨY LÊN GITHUB (FORCE PUSH)
    print("[*] Dang day du lieu len GitHub, vui long cho...")
    try:
        project_dir = env.get("PROJECT_DIR")
        
        # Thêm các file thay đổi
        subprocess.run(["git", "add", "."], check=True, cwd=project_dir)
        
        # Tạo Commit
        commit_msg = f"Auto-Upload OTA: {version} - {fw_name}"
        subprocess.run(["git", "commit", "-m", commit_msg], check=True, cwd=project_dir)
        
        # CÚ CHỐT: Dùng thêm cờ "-f" để ép GitHub nhận file, bỏ qua lỗi lệch phiên bản
        subprocess.run(["git", "push", "-f", "origin", "main"], check=True, cwd=project_dir)
        
        print("\033[92m[+] TAI LEN GITHUB THANH CONG!\033[0m")
    except Exception as e:
        print(f"\033[91m[-] Loi tai len GitHub: {e}\033[0m")

    print("\033[92m=======================================\033[0m\n")

# Kích hoạt script chạy ngay sau khi tạo xong file .bin
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)