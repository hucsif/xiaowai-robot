#!/usr/bin/env python3
"""Deskbot 串口监控：带墙钟时间戳、落盘存档、断线/拔插自动重连。

用法：
  python3 serial_monitor.py                      # 自动找 /dev/ttyACM* 或 /dev/ttyUSB*
  python3 serial_monitor.py --port /dev/ttyACM0  # 指定端口
  python3 serial_monitor.py --baud 115200        # 波特率（默认 115200）
  python3 serial_monitor.py --log my.log         # 指定日志文件（默认按时间戳命名）
  python3 serial_monitor.py --no-log             # 只打印到终端，不写文件

提示：启动后按板子 RST/EN 可回放开机日志。Ctrl+C 退出。
"""

import argparse
import datetime
import glob
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial，请先安装：pip install pyserial")


def find_port():
    """返回一个存在的串口（优先刚插入的 /dev/ttyACM*，其次 /dev/ttyUSB*）；无则 None。"""
    candidates = []
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        candidates.extend(glob.glob(pattern))
    if not candidates:
        return None
    # 按修改时间最新优先：刚插上的板子通常是我们要的
    candidates.sort(key=os.path.getmtime, reverse=True)
    return candidates[0]


def open_serial(port, baud):
    ser = serial.Serial(port, baud, timeout=0.5)
    ser.reset_input_buffer()  # 丢掉打开瞬间残留的半截缓冲，避免乱码
    return ser


def main():
    ap = argparse.ArgumentParser(description="Deskbot 串口监控")
    ap.add_argument("--port", default=None,
                    help="串口设备；默认自动检测 /dev/ttyACM*、/dev/ttyUSB*")
    ap.add_argument("--baud", type=int, default=115200, help="波特率（默认 115200）")
    ap.add_argument("--log", default=None,
                    help="日志文件路径，默认 serial_YYYYMMDD_HHMMSS.log")
    ap.add_argument("--no-log", action="store_true", help="只打印到终端，不写文件")
    args = ap.parse_args()

    # 日志文件
    log_path = None
    log_file = None
    if not args.no_log:
        log_path = args.log or ("serial_%s.log"
                                % datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
        log_file = open(log_path, "a", buffering=1)

    def emit(line):
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        out = "[%s] %s" % (ts, line)
        print(out, end="")
        if log_file:
            log_file.write(out)
            log_file.flush()

    port = args.port
    print("[monitor] port=%s baud=%d log=%s" % (port or "(auto)", args.baud, log_path or "(none)"))
    print("[monitor] Ctrl+C 退出；如要回放开机日志，请按板子 RST/EN")

    ser = None
    try:
        while True:
            # 自动检测模式下每次刷新端口，兼容拔插后 ttyACM0 -> ttyACM1
            if args.port is None:
                port = find_port()
            if not port:
                time.sleep(2)
                continue

            if ser is None:
                try:
                    ser = open_serial(port, args.baud)
                    emit("[monitor] connected %s\n" % port)
                except (serial.SerialException, OSError):
                    time.sleep(2)
                    continue

            try:
                b = ser.readline()
                if b:
                    emit(b.decode("utf-8", errors="replace"))
            except (serial.SerialException, OSError):
                emit("[monitor] disconnected, retrying...\n")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(1)
    except KeyboardInterrupt:
        print("\n[monitor] stopped")
    finally:
        if ser:
            try:
                ser.close()
            except Exception:
                pass
        if log_file:
            log_file.close()


if __name__ == "__main__":
    main()
