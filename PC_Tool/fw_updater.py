"""
STM32 固件升级工具 v2.0 — 串口终端 + 升级一体
"""
import tkinter as tk
from tkinter import filedialog, ttk, scrolledtext
import threading, serial, serial.tools.list_ports, struct, os, time, binascii
from datetime import datetime
from protocol import *

# ---------------------------- 升级线程 ----------------------------
class UpdateWorker(threading.Thread):
    def __init__(self, ser, filepath, log_cb, progress_cb, status_cb, done_cb, term_cb):
        super().__init__(daemon=True)
        self.ser = ser
        self.filepath = filepath
        self.log = log_cb
        self.progress = progress_cb
        self.status = status_cb
        self.done = done_cb
        self.term = term_cb
        self.stop_flag = False

    def stop(self): self.stop_flag = True

    def _ts(self): return datetime.now().strftime('%H:%M:%S.') + f'{datetime.now().microsecond // 1000:03d}'

    def _wait_pkt(self, expected, timeout=5.0):
        parser = FrameParser()
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.stop_flag: return None
            b = self.ser.read(1)
            if b:
                r = parser.feed(b[0])
                if r:
                    if r['type'] == expected: return r
                    elif r['type'] == PKT_STATUS:
                        self.log(f"← MCU: {r['payload'].decode('utf-8','replace').strip()}")
                    elif r['type'] == PKT_NAK:
                        self.log(f"← NAK err={r['payload'][0] if r['payload'] else '?'}")
                    elif r['type'] == PKT_READY:
                        self.log(f"← READY (重复)")
        return None

    def run(self):
        try:
            fw = open(self.filepath, 'rb').read()
            total = len(fw)
            crc32 = binascii.crc32(fw) & 0xFFFFFFFF
            name = os.path.basename(self.filepath)
            self.log(f"固件: {name} | {total:,}B | CRC32=0x{crc32:08X}")
            self.status("等待 MCU 就绪...")
            self.log("◇ 等待 PKT_READY (MCU 每 3 秒自动重发)")

            # Step 1: wait READY
            rdy = self._wait_pkt(PKT_READY, timeout=65)
            if not rdy:
                self.log("⚠ 超时: 未收到 PKT_READY")
                self.log("⚠ 排查: ① 串口线 TX/RX 是否交叉? ② MCU 是否正在运行?")
                self.done(False); return
            self.log(f"← PKT_READY 收到 ✓")

            # Step 2: send INFO
            self.log(f"→ PKT_INFO (size={total}, CRC32=0x{crc32:08X})")
            self.ser.write(build_frame(PKT_INFO, 0, struct.pack('>II', total, crc32)))
            ack = self._wait_pkt(PKT_ACK, timeout=3)
            if not ack:
                self.log("⚠ 未收到 ACK (PKT_INFO)")
                self.log("⚠ 排查: MCU UART RX 是否正常? 中断回调是否触发?")
                self.done(False); return
            self.log(f"← ACK seq={ack['seq']}")

            # Step 3: send DATA blocks
            seq = 0; sent = 0; CHUNK = 256
            self.log(f"→ 开始发送数据 ({CHUNK}B/块, 共 {total} 字节)")
            self.status("发送中...")

            while sent < total:
                if self.stop_flag: self.log("⚠ 中止"); self.done(False); return
                chunk = fw[sent:sent+CHUNK]
                frame = build_frame(PKT_DATA, seq, chunk)
                for retry in range(3):
                    self.ser.write(frame)
                    ack = self._wait_pkt(PKT_ACK, timeout=2)
                    if ack: break
                    self.log(f"⚠ seq={seq} 无 ACK, 重试 {retry+2}/3")
                if not ack: self.log(f"⚠ seq={seq} 失败"); self.done(False); return
                sent += len(chunk); seq += 1
                pct = int(sent*100/total)
                self.progress(pct)
                if sent % 4096 < CHUNK: self.log(f"→ {sent}/{total} ({pct}%)")

            # Step 4: COMPLETE
            self.log("→ PKT_COMPLETE")
            self.ser.write(build_frame(PKT_COMPLETE, 0))
            ok = self._wait_pkt(PKT_OK, timeout=15)
            if ok:
                self.log("← PKT_OK — 升级完成 ✓")
                self.progress(100); self.status("升级成功!")
            else:
                self.log("⚠ 未收到 PKT_OK"); self.status("升级失败")
            self.done(ok is not None)
        except Exception as e:
            self.log(f"⚠ 异常: {e}"); self.done(False)


# ---------------------------- 终端监听 ----------------------------
class SerialTerminal(threading.Thread):
    def __init__(self, ser, cb):
        super().__init__(daemon=True)
        self.ser = ser; self.cb = cb; self.running = True

    def stop(self): self.running = False

    def run(self):
        buf = bytearray()
        while self.running and self.ser and self.ser.is_open:
            try:
                b = self.ser.read(1)
                if not b: continue
                val = b[0]
                if val == 0x0A:           # \n → 一行结束
                    self.cb(bytes(buf), complete=True)
                    buf.clear()
                elif val == 0x0D:         # \r → 跳过
                    pass
                elif 0x20 <= val <= 0x7E or val in (0x09,):  # 可打印 + TAB
                    buf.append(val)
                else:
                    buf.append(0x2E)      # . 替换不可打印字符
            except: break


# ---------------------------- GUI ----------------------------
class FWUpdaterApp:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("STM32 固件升级工具 v2.0")
        self.root.geometry("860x680")
        self.root.resizable(True, True)
        self.ser = None; self.worker = None; self.terminal = None; self.filepath = ""
        self._build(); self._refresh_ports()

    # ---- 构建 UI ----
    def _build(self):
        # 顶部控制栏
        bar = ttk.Frame(self.root); bar.pack(fill='x', padx=8, pady=4)
        ttk.Label(bar, text="串口:").pack(side='left')
        self.combo_port = ttk.Combobox(bar, width=10, state='readonly'); self.combo_port.pack(side='left',padx=2)
        ttk.Label(bar, text="波特率:").pack(side='left')
        self.combo_baud = ttk.Combobox(bar, width=8, values=['9600','19200','38400','115200','256000'], state='readonly')
        self.combo_baud.set('115200'); self.combo_baud.pack(side='left',padx=2)
        self.btn_open = ttk.Button(bar, text="打开串口", command=self._toggle); self.btn_open.pack(side='left',padx=2)
        self.var_term = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="终端", variable=self.var_term).pack(side='left',padx=5)
        ttk.Button(bar, text="刷新端口", command=self._refresh_ports).pack(side='left',padx=2)
        ttk.Button(bar, text="清屏", command=self._clear).pack(side='right',padx=2)

        # 文件栏
        fbar = ttk.Frame(self.root); fbar.pack(fill='x', padx=8, pady=2)
        self.entry_file = ttk.Entry(fbar); self.entry_file.pack(side='left',fill='x',expand=True, padx=(0,5))
        ttk.Button(fbar, text="选择固件", command=self._browse).pack(side='right')
        self.lbl_file = ttk.Label(fbar, text="未选择", foreground='gray'); self.lbl_file.pack(side='bottom',anchor='w')

        # 升级按钮
        bbar = ttk.Frame(self.root); bbar.pack(fill='x', padx=8, pady=2)
        self.btn_up = ttk.Button(bbar, text="开始升级", command=self._start_update, state='disabled'); self.btn_up.pack(side='left',padx=2)
        self.btn_stop = ttk.Button(bbar, text="停止", command=self._stop_update, state='disabled'); self.btn_stop.pack(side='left',padx=2)
        self.progress = ttk.Progressbar(bbar, length=300, mode='determinate'); self.progress.pack(side='left',fill='x',expand=True,padx=10)
        self.lbl_status = ttk.Label(bbar, text="就绪", width=22); self.lbl_status.pack(side='right')

        # 终端
        self.term = scrolledtext.ScrolledText(self.root, font=('Consolas',10), bg='#1E1E1E', fg='#D4D4D4',
                                               insertbackground='white', state='disabled', wrap='char')
        self.term.pack(fill='both', expand=True, padx=8, pady=4)
        for tag,clr in [('tx','#569CD6'),('rx','#6A9955'),('warn','#CE9178'),('ok','#4EC9B0'),('ts','#808080')]:
            self.term.tag_config(tag, foreground=clr)

    # ---- 方法 ----
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.combo_port['values'] = ports
        if ports and not self.combo_port.get(): self.combo_port.set(ports[0])

    def _toggle(self):
        if self.ser and self.ser.is_open:
            if self.terminal: self.terminal.stop(); self.terminal = None
            self.ser.close(); self.btn_open.config(text="打开串口")
            self._term_write("[串口已关闭]\n", 'ts'); self.btn_up.config(state='disabled')
        else:
            try:
                self.ser = serial.Serial(self.combo_port.get(), int(self.combo_baud.get()), timeout=0.05)
                self.btn_open.config(text="关闭串口")
                self._term_write(f"[串口 {self.combo_port.get()} @ {self.combo_baud.get()} 已打开]\n", 'ts')
                if self.filepath: self.btn_up.config(state='normal')
                if self.var_term.get():
                    self.terminal = SerialTerminal(self.ser, self._on_term_line); self.terminal.start()
            except Exception as e:
                self._term_write(f"[打开串口失败: {e}]\n", 'warn')

    def _browse(self):
        p = filedialog.askopenfilename(filetypes=[("BIN","*.bin"),("所有","*.*")])
        if p:
            self.filepath = p; sz = os.path.getsize(p)
            self.entry_file.delete(0,tk.END); self.entry_file.insert(0,os.path.basename(p))
            self.lbl_file.config(text=f"{os.path.basename(p)} — {sz:,} 字节", foreground='white')
            if self.ser and self.ser.is_open: self.btn_up.config(state='normal')

    def _term_write(self, text, tag=None):
        def _do():
            self.term.configure(state='normal')
            self.term.insert(tk.END, text, tag or ())
            self.term.see(tk.END)
            self.term.configure(state='disabled')
        self.root.after(0, _do)

    def _on_term_line(self, data: bytes, complete=False):
        """收到一行时，加时间戳显示"""
        ts = datetime.now().strftime('[%H:%M:%S.') + f'{datetime.now().microsecond // 1000:03d}] '
        text = data.decode('ascii', errors='replace').replace('\r','')
        self._term_write(f"{ts}{text}\n")

    def _clear(self):
        self.term.configure(state='normal'); self.term.delete('1.0',tk.END); self.term.configure(state='disabled')

    def log(self, msg, tag=None):
        """升级日志"""
        ts = datetime.now().strftime('%H:%M:%S.') + f'{datetime.now().microsecond // 1000:03d}'
        t = tag or ''
        if msg.startswith('→'): t = 'tx'
        elif msg.startswith('←'): t = 'rx'
        elif msg.startswith('⚠'): t = 'warn'
        self._term_write(f"[{ts}] {msg}\n", t)

    def _start_update(self):
        if not self.ser: return self.log("⚠ 请先打开串口")
        if not self.filepath: return self.log("⚠ 请选择固件文件")
        if self.terminal: self.terminal.stop(); self.terminal = None
        self.btn_up.config(state='disabled'); self.btn_stop.config(state='normal')
        self.progress['value'] = 0
        self.worker = UpdateWorker(ser=self.ser, filepath=self.filepath,
            log_cb=self.log, progress_cb=lambda p:(self.progress.configure(value=p),self.root.update_idletasks()),
            status_cb=lambda s:self.lbl_status.config(text=s), done_cb=self._done, term_cb=self._on_term_line)
        self.worker.start()

    def _stop_update(self):
        if self.worker: self.worker.stop(); self.log("⚠ 用户中止")

    def _done(self, ok):
        self.btn_up.config(state='normal'); self.btn_stop.config(state='disabled')
        self.lbl_status.config(text="升级完成 ✓" if ok else "升级失败 ✗")
        if ok: self.progress['value'] = 100
        self.worker = None
        if self.ser and self.ser.is_open and self.var_term.get():
            self.terminal = SerialTerminal(self.ser, self._on_term_line); self.terminal.start()

    def run(self): self.root.mainloop()


if __name__ == '__main__': app = FWUpdaterApp(); app.run()
