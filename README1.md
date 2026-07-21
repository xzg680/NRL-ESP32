Set-ExecutionPolicy -Scope Process Bypass -Force
$env:PYTHONUTF8 = "1"
D:\esp\v6.1.0\esp-idf\export.ps1
python scripts/build.py bi4umd build flash -p COM5

python scripts/build.py bi4umd build flash monitor -p COM5   # 编译、烧录并监视
python scripts/build.py bi4umd menuconfig                    # 修改配置
