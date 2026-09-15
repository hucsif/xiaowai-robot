#!/usr/bin/env python3
"""Deskbot 串口监控：带墙钟时间戳、落盘存档、断线/拔插自动重连。

用法：
  python3 serial_monitor.py                      # 自动找 /dev/ttyACM* 或 /dev/ttyUSB*
  python3 serial_monitor.py --port /dev/ttyACM0  # 指定端口
  python3 serial_monitor.py --baud 115200        # 波特率（默认 115200）
  python3 serial_monitor.py --log my.log         # 指定日志文件（覆盖 --log-dir）
  python3 serial_monitor.py --no-log             # 只打印到终端，不写文件
  python3 serial_monitor.py --keep-days 3        # 只保留最近 3 天（0=不清理）

日志默认写到脚本同级的 logs/ 子目录（不再堆在项目根目录），
并在每次启动时自动清理超期的旧日志。

提示：启动后按板子 RST/EN 可回放开局日志。Ctrl+C 退出。
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


# 日志落到脚本同级的 logs/，而不是「运行时的当前目录」——否则在哪儿跑就堆在哪儿
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOG_DIR = os.path.join(SCRIPT_DIR, "logs")
DEFAULT_KEEP_DAYS = 7


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


def prune_old_logs(directories, keep_days):
    """删除给定目录下超过 keep_days 天的 serial_*.log。

    返回 (删除数, 释放字节数)。keep_days <= 0 表示不清理。
    """
    if keep_days <= 0:
        return 0, 0
    cutoff = time.time() - keep_days * 86400
    removed = 0
    freed = 0
    for d in directories:
        for path in glob.glob(os.path.join(d, "serial_*.log")):
            try:
                if os.path.getmtime(path) >= cutoff:
                    continue
                size = os.path.getsize(path)
                os.remove(path)
                removed += 1
                freed += size
            except OSError:
                pass  # 权限/竞态等：跳过，不影响监控本身
    return removed, freed


def main():
    ap = argparse.ArgumentParser(description="Deskbot 串口监控")
    ap.add_argument("--port", default=None,
                    help="串口设备；默认自动检测 /dev/ttyACM*、/dev/ttyUSB*")
    ap.add_argument("--baud", type=int, default=115200, help="波特率（默认 115200）")
    ap.add_argument("--log", default=None,
                    help="日志文件路径；指定后忽略 --log-dir")
    ap.add_argument("--log-dir", default=DEFAULT_LOG_DIR,
                    help="日志目录（默认 <脚本目录>/logs）")
    ap.add_argument("--keep-days", type=int, default=DEFAULT_KEEP_DAYS,
                    help="启动时清理超过 N 天的旧日志（默认 %d；0=不清理）"
                         % DEFAULT_KEEP_DAYS)
    ap.add_argument("--no-log", action="store_true", help="只打印到终端，不写文件")
    args = ap.parse_args()

    # 日志文件
    log_path = None
    log_file = None
    if not args.no_log:
        if args.log:
            log_path = args.log
        else:
            # 历史遗留：早期版本把日志写在脚本目录而非 logs/ 子目录，
            # 一并纳入清理范围，免得那批旧文件永远留着
            pruned, freed = prune_old_logs(
                [args.log_dir, SCRIPT_DIR], args.keep_days
            )
            if pruned:
                print("[monitor] 清理了 %d 个超期日志（%d 天前，释放 %.1f KB）"
                      % (pruned, args.keep_days, freed / 1024.0))
            os.makedirs(args.log_dir, exist_ok=True)
            log_path = os.path.join(
                args.log_dir,
                "serial_%s.log" % datetime.datetime.now().strftime("%Y%m%d_%H%M%S"),
            )
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
