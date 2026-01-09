import os
import shlex
import threading
import subprocess
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import re
from collections import defaultdict

DEFAULT_BINARY = './out/blitzping'

class BlitzpingGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('Blitzping GUI')
        self.geometry('1100x600')

        self.proc = None
        # store per-hop stats: {hop: {'ip': str, 'count': int, 'sum': float, 'last': float}}
        self.hops = defaultdict(lambda: {'ip': None, 'count': 0, 'sum': 0.0, 'last': None})
        self._build_ui()

        # regexes to detect hop, ip, rtt; these are tolerant and cover several output styles
        self._ip_re = re.compile(r'(?:(?:\d{1,3}\.){3}\d{1,3})')
        # common "1 10.0.0.1 0.123 ms" or " 1 10.0.0.1  0.123 ms"
        self._hop_line_re = re.compile(r'^\s*(\d+)\s+([0-9\.]+)\b.*?([0-9]*\.?[0-9]+)\s*ms', re.IGNORECASE)
        # "Hop 1: ip=10.0.0.1 rtt=0.12ms" or "hop=1 ip=... time=..."
        self._keyval_re = re.compile(r'(?:hop[:=]\s*(\d+))|(?:ttl[:=]\s*(\d+))|(?:hop\s+(\d+))', re.IGNORECASE)
        self._ip_kv_re = re.compile(r'ip[:=]\s*(' + r'(?:\d{1,3}\.){3}\d{1,3}' + r')', re.IGNORECASE)
        self._rtt_kv_re = re.compile(r'(?:rtt|time)[:=]\s*([0-9]*\.?[0-9]+)\s*ms', re.IGNORECASE)
        # fallback: find ip and any "time=123.45 ms" anywhere
        self._time_re = re.compile(r'time[:=]?\s*([0-9]*\.?[0-9]+)\s*ms', re.IGNORECASE)

    def _build_ui(self):
        frm = ttk.Frame(self, padding=8)
        frm.pack(fill='both', expand=True)

        top = ttk.Frame(frm)
        top.pack(fill='x')

        # Binary selection
        ttk.Label(top, text='blitzping binary:').grid(row=0, column=0, sticky='w')
        self.bin_var = tk.StringVar(value=DEFAULT_BINARY)
        bin_entry = ttk.Entry(top, textvariable=self.bin_var, width=60)
        bin_entry.grid(row=0, column=1, sticky='w')
        ttk.Button(top, text='Browse', command=self._browse_binary).grid(row=0, column=2, padx=6)

        # Run as sudo toggle
        self.sudo_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(top, text='Run with sudo (or run GUI with sudo)', variable=self.sudo_var).grid(row=0, column=3, padx=6)

        ttk.Label(top, text='Destination IP:').grid(row=1, column=0, sticky='w', pady=(8,0))
        self.dest_var = tk.StringVar(value='8.8.8.8')
        ttk.Entry(top, textvariable=self.dest_var, width=18).grid(row=1, column=1, sticky='w', pady=(8,0))

        ttk.Label(top, text='Num threads:').grid(row=1, column=2, sticky='w', pady=(8,0), padx=(8,0))
        self.threads_var = tk.StringVar(value='1')
        ttk.Entry(top, textvariable=self.threads_var, width=8).grid(row=1, column=3, sticky='w', pady=(8,0))

        ttk.Label(top, text='Source IP:').grid(row=1, column=4, sticky='w', padx=(12,0))
        self.src_ip_var = tk.StringVar(value='')
        ttk.Entry(top, textvariable=self.src_ip_var, width=14).grid(row=1, column=5, sticky='w')

        ttk.Label(top, text='Gate MAC:').grid(row=1, column=6, sticky='w', padx=(12,0))
        self.gatemac_var = tk.StringVar(value='')
        ttk.Entry(top, textvariable=self.gatemac_var, width=18).grid(row=1, column=7, sticky='w')

        self.tracert_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(top, text='Traceroute (--tracert)', variable=self.tracert_var).grid(row=1, column=8, padx=6)

        # DPDK options
        dpdk_frame = ttk.Labelframe(frm, text='DPDK options', padding=8)
        dpdk_frame.pack(fill='x', pady=(10,0))

        self.use_dpdk_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(dpdk_frame, text='Use DPDK (--use-dpdk)', variable=self.use_dpdk_var).grid(row=0, column=0, sticky='w')

        ttk.Label(dpdk_frame, text='-l (core list):').grid(row=0, column=1, sticky='w', padx=(12,0))
        self.corelist_var = tk.StringVar(value='0')
        ttk.Entry(dpdk_frame, textvariable=self.corelist_var, width=10).grid(row=0, column=2, sticky='w')

        ttk.Label(dpdk_frame, text='-a (PCI address):').grid(row=0, column=3, sticky='w', padx=(12,0))
        self.pci_var = tk.StringVar(value='0000:38:00.0')
        ttk.Entry(dpdk_frame, textvariable=self.pci_var, width=18).grid(row=0, column=4, sticky='w')

        ttk.Label(dpdk_frame, text='Extra DPDK args:').grid(row=1, column=0, sticky='w', pady=(8,0))
        self.extra_dpdk_var = tk.StringVar(value='')
        ttk.Entry(dpdk_frame, textvariable=self.extra_dpdk_var, width=60).grid(row=1, column=1, columnspan=4, sticky='w', pady=(8,0))

        # Extra args
        args_frame = ttk.Frame(frm)
        args_frame.pack(fill='x', pady=(10,0))
        ttk.Label(args_frame, text='Additional blitzping args:').grid(row=0, column=0, sticky='w')
        self.extra_args_var = tk.StringVar(value='')
        ttk.Entry(args_frame, textvariable=self.extra_args_var, width=90).grid(row=1, column=0, sticky='w')

        # Buttons
        btns = ttk.Frame(frm)
        btns.pack(fill='x', pady=(12,0))
        self.start_btn = ttk.Button(btns, text='Start', command=self.start)
        self.start_btn.pack(side='left')
        self.stop_btn = ttk.Button(btns, text='Stop', command=self.stop, state='disabled')
        self.stop_btn.pack(side='left', padx=(8,0))
        ttk.Button(btns, text='Clear Output', command=self._clear_output).pack(side='right')

        # Main split: left = raw output, right = network path
        main_pane = ttk.Panedwindow(frm, orient='horizontal')
        main_pane.pack(fill='both', expand=True, pady=(10,0))

        # Output (left)
        out_frame = ttk.Frame(main_pane)
        main_pane.add(out_frame, weight=3)
        ttk.Label(out_frame, text='Output:').pack(anchor='w')
        self.text = tk.Text(out_frame, wrap='none')
        self.text.pack(fill='both', expand=True)

        # Scrollbars for output
        ysb = ttk.Scrollbar(self.text, orient='vertical', command=self.text.yview)
        self.text['yscrollcommand'] = ysb.set
        ysb.pack(side='right', fill='y')

        # Path (right)
        path_frame = ttk.Labelframe(main_pane, text='Network Path (real-time)', padding=8)
        main_pane.add(path_frame, weight=1)
        # Treeview for hops
        columns = ('hop', 'ip', 'last_ms', 'avg_ms', 'count')
        self.hop_tree = ttk.Treeview(path_frame, columns=columns, show='headings', height=20)
        self.hop_tree.heading('hop', text='Hop')
        self.hop_tree.heading('ip', text='IP')
        self.hop_tree.heading('last_ms', text='Last (ms)')
        self.hop_tree.heading('avg_ms', text='Avg (ms)')
        self.hop_tree.heading('count', text='Count')
        self.hop_tree.column('hop', width=50, anchor='center')
        self.hop_tree.column('ip', width=120, anchor='w')
        self.hop_tree.column('last_ms', width=80, anchor='e')
        self.hop_tree.column('avg_ms', width=80, anchor='e')
        self.hop_tree.column('count', width=60, anchor='center')
        self.hop_tree.pack(fill='both', expand=True)

        hop_btns = ttk.Frame(path_frame)
        hop_btns.pack(fill='x', pady=(6,0))
        ttk.Button(hop_btns, text='Clear Paths', command=self._clear_paths).pack(side='left')

    def _browse_binary(self):
        p = filedialog.askopenfilename(title='Select blitzping binary', initialfile=self.bin_var.get())
        if p:
            self.bin_var.set(p)

    def _build_command(self):
        bin_path = self.bin_var.get().strip()
        if not bin_path:
            raise ValueError('blitzping binary path is empty')
        if not os.path.isfile(bin_path):
            raise ValueError(f'Binary not found: {bin_path}')

        # We'll construct the command differently depending on whether DPDK/EAL args are needed.
        cmd = []
        if self.sudo_var.get():
            cmd.append('sudo')
        cmd.append(bin_path)

        dest = self.dest_var.get().strip()
        threads = self.threads_var.get().strip()
        src_ip = self.src_ip_var.get().strip()
        gatemac = self.gatemac_var.get().strip()

        # If using DPDK, EAL options (-l, -a, and other EAL flags) must appear BEFORE app args.
        if self.use_dpdk_var.get():
            # Add EAL options
            l = self.corelist_var.get().strip()
            a = self.pci_var.get().strip()
            if l:
                cmd.extend(['-l', l])
            if a:
                cmd.extend(['-a', a])

            extra_dpdk = shlex.split(self.extra_dpdk_var.get() or '')
            if extra_dpdk:
                cmd.extend(extra_dpdk)

            # Now add the '--' separator and application arguments (app sees these)
            cmd.append('--')

            # Ensure application-level --use-dpdk exists so blitzping knows to enable dpdk mode
            cmd.append('--use-dpdk')

            if dest:
                cmd.append(f'--dest-ip={dest}')
            if threads:
                cmd.append(f'--num-threads={threads}')
            if src_ip:
                cmd.append(f'--src-ip={src_ip}')
            if gatemac:
                cmd.append(f'--gate-mac={gatemac}')

        else:
            # Non-DPDK flow: pass application args directly
            if dest:
                cmd.append(f'--dest-ip={dest}')
            if threads:
                cmd.append(f'--num-threads={threads}')
            if src_ip:
                cmd.append(f'--src-ip={src_ip}')
            if gatemac:
                cmd.append(f'--gate-mac={gatemac}')

        # Traceroute
        if self.tracert_var.get():
            cmd.append('--tracert')

        # Additional args the user typed
        extra = shlex.split(self.extra_args_var.get() or '')
        if extra:
            cmd.extend(extra)

        return cmd

    def start(self):
        try:
            cmd = self._build_command()
        except Exception as e:
            messagebox.showerror('Invalid command', str(e))
            return

        if self.proc is not None:
            messagebox.showwarning('Already running', 'A blitzping process is already running')
            return

        self._append_output(f'Running: {shlex.join(cmd)}\n\n')

        try:
            # Start process
            self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        except Exception as e:
            self._append_output(f'Failed to start process: {e}\n')
            self.proc = None
            return

        # UI state
        self.start_btn['state'] = 'disabled'
        self.stop_btn['state'] = 'normal'

        # Thread to read output
        t = threading.Thread(target=self._read_process_output, daemon=True)
        t.start()

    def _read_process_output(self):
        try:
            for line in self.proc.stdout:
                self._append_output(line)
                # also try to parse for hop info
                self.after(0, lambda ln=line: self._process_line_for_hops(ln))
        except Exception as e:
            self._append_output(f'Error reading process output: {e}\n')
        finally:
            rc = self.proc.wait()
            self._append_output(f'\nProcess exited with return code {rc}\n')
            self.proc = None
            self.start_btn['state'] = 'normal'
            self.stop_btn['state'] = 'disabled'

    def stop(self):
        if not self.proc:
            return
        try:
            self._append_output('\nTerminating process...\n')
            self.proc.terminate()
            # Give it a short time, then kill
            threading.Timer(2.0, self._kill_if_needed).start()
        except Exception as e:
            self._append_output(f'Failed to terminate process: {e}\n')

    def _kill_if_needed(self):
        if self.proc:
            try:
                self._append_output('Killing process...\n')
                self.proc.kill()
            except Exception as e:
                self._append_output(f'Failed to kill process: {e}\n')

    def _append_output(self, s):
        # Insert into text widget from main thread
        def _insert():
            self.text.insert('end', s)
            self.text.see('end')
        self.after(0, _insert)

    def _clear_output(self):
        self.text.delete('1.0', 'end')

    def _clear_paths(self):
        self.hops.clear()
        for item in self.hop_tree.get_children():
            self.hop_tree.delete(item)

    def _process_line_for_hops(self, line: str):
        """
        Best-effort parsing for hop info and RTT in ms.
        Updates self.hops and the Treeview with aggregated statistics.
        """
        line = line.strip()
        if not line:
            return

        # 1) Try match the simple 'hop ip rtt ms' format
        m = self._hop_line_re.match(line)
        if m:
            hop = int(m.group(1))
            ip = m.group(2)
            try:
                rtt = float(m.group(3))
            except:
                rtt = None
            if rtt is not None:
                self._update_hop(hop, ip, rtt)
            return

        # 2) Try key-value style: ip=..., rtt=... hop: N or "Hop N:"
        hop = None
        ip = None
        rtt = None

        # look for explicit "Hop N" or hop= / ttl=
        m_hop = re.search(r'(?:Hop[:\s]+(\d+))|(?:\bhop[:=]\s*(\d+))|(?:\bttl[:=]\s*(\d+))', line, re.IGNORECASE)
        if m_hop:
            for g in m_hop.groups():
                if g:
                    hop = int(g)
                    break

        m_ip = self._ip_kv_re.search(line)
        if m_ip:
            ip = m_ip.group(1)
        else:
            # fallback: any IP in line
            m_anyip = self._ip_re.search(line)
            if m_anyip:
                ip = m_anyip.group(0)

        m_rtt = self._rtt_kv_re.search(line)
        if m_rtt:
            try:
                rtt = float(m_rtt.group(1))
            except:
                rtt = None
        else:
            m_time = self._time_re.search(line)
            if m_time:
                try:
                    rtt = float(m_time.group(1))
                except:
                    rtt = None

        # If we have at least hop and rtt, update
        if hop is not None and rtt is not None:
            if ip is None:
                # try to guess ip from line
                m_anyip = self._ip_re.search(line)
                if m_anyip:
                    ip = m_anyip.group(0)
            self._update_hop(hop, ip, rtt)
            return

        # 3) fallback: sometimes lines like "1 192.168.1.1 1.23 ms 2.34 ms 3.45 ms"
        # try to capture first number as hop, first ip, and first time occurrence
        tokens = line.split()
        if len(tokens) >= 3:
            # find first token that looks like an integer (hop)
            maybe_hop = None
            try:
                maybe_hop = int(tokens[0])
            except:
                # not in position 0; search
                for t in tokens[:3]:
                    if t.isdigit():
                        maybe_hop = int(t)
                        break
            if maybe_hop is not None:
                found_ip = None
                for t in tokens:
                    if self._ip_re.match(t):
                        found_ip = t
                        break
                # find first token that ends with 'ms' or next token that looks like a float and 'ms' follows
                found_rtt = None
                for i, t in enumerate(tokens):
                    if t.lower().endswith('ms'):
                        try:
                            val = float(t[:-2])
                            found_rtt = val
                            break
                        except:
                            continue
                    # pattern: '0.123' followed by 'ms'
                    if i+1 < len(tokens) and tokens[i+1].lower() == 'ms':
                        try:
                            val = float(t)
                            found_rtt = val
                            break
                        except:
                            continue
                if found_rtt is not None:
                    self._update_hop(maybe_hop, found_ip, found_rtt)
                    return

        # Nothing matched: ignore the line for path parsing
        return

    def _update_hop(self, hop: int, ip: str, rtt: float):
        """
        Update internal stats and Treeview row for the hop.
        """
        if ip is None:
            ip = '-'
        entry = self.hops[hop]
        # if IP was previously None, set it; if different, overwrite but keep history
        entry['ip'] = ip
        entry['count'] += 1
        entry['sum'] += rtt
        entry['last'] = rtt
        avg = entry['sum'] / entry['count']

        # update treeview (create row if not present)
        def _update_row():
            key = str(hop)
            # find existing item for this hop (search by tag or by value)
            item_id = None
            for it in self.hop_tree.get_children():
                vals = self.hop_tree.item(it, 'values')
                if vals and str(vals[0]) == key:
                    item_id = it
                    break
            if item_id is None:
                # insert in hop order: find position to insert
                # we'll append and allow sorting by hop in UI if needed
                self.hop_tree.insert('', 'end', values=(hop, ip, f'{rtt:.3f}', f'{avg:.3f}', entry['count']))
            else:
                self.hop_tree.item(item_id, values=(hop, ip, f'{rtt:.3f}', f'{avg:.3f}', entry['count']))
        self.after(0, _update_row)


if __name__ == '__main__':
    app = BlitzpingGUI()
    app.mainloop()
