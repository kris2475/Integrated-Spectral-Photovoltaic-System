import threading
import queue
import time
import csv
from datetime import datetime
import serial
import serial.tools.list_ports
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
import tkinter as tk
from tkinter import ttk, filedialog

data_queue = queue.Queue()

class SciFiDashboard:
    def __init__(self, root):
        self.root = root
        self.root.title("Solar & Spectrometer Monitoring Deck")
        self.root.geometry("1350x900")
        self.root.configure(bg="#050811")

        # Pre-calculated wavelength array for display/CSV export
        pix_indices = np.arange(288, dtype=float)
        A0, B1, B2, B3, B4, B5 = 3.02539467e+02, 2.70472314e+00, -1.10776346e-03, -8.62290205e-06, 1.42568072e-08, -2.66396197e-12
        self.wavelengths = (A0 + B1 * pix_indices + B2 * (pix_indices**2) + 
                            B3 * (pix_indices**3) + B4 * (pix_indices**4) + B5 * (pix_indices**5))

        # Data Buffers
        self.timestamps = []
        self.voltage_history = []
        self.current_history = []
        self.power_history = []
        self.spectrum = np.zeros(288)

        self.time_mode = "rolling" 
        
        self.running = True
        self.ser = None
        self.is_connected = False
        self.is_recording = False
        self.csv_file = None
        self.csv_writer = None

        self.setup_styles()
        self.setup_ui()

        self.serial_thread = threading.Thread(target=self.read_serial_loop, daemon=True)
        self.serial_thread.start()

        self.update_dashboard()

    def setup_styles(self):
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TFrame", background="#050811")
        style.configure("TLabel", background="#050811", foreground="#00f3ff", font=("Consolas", 10))

    def setup_ui(self):
        header_frame = ttk.Frame(self.root, padding=10)
        header_frame.pack(side=tk.TOP, fill=tk.X)

        title_lbl = ttk.Label(header_frame, text="SYSTEM MONITOR: MPPT & SPECTRAL TELEMETRY", font=("Consolas", 13, "bold"))
        title_lbl.pack(side=tk.LEFT, padx=5)

        self.status_lbl = ttk.Label(header_frame, text="STATUS: OFFLINE", foreground="#ff0055")
        self.status_lbl.pack(side=tk.RIGHT, padx=10)

        control_frame = ttk.Frame(self.root, padding=10)
        control_frame.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(control_frame, text="PORT:").pack(side=tk.LEFT, padx=2)
        
        self.port_combobox = ttk.Combobox(control_frame, width=12, state="readonly", font=("Consolas", 10))
        self.port_combobox.pack(side=tk.LEFT, padx=5)
        self.refresh_ports()

        refresh_btn = tk.Button(control_frame, text="🔄", command=self.refresh_ports, 
                                bg="#0b1329", fg="#00f3ff", font=("Consolas", 9), relief="solid", bd=1)
        refresh_btn.pack(side=tk.LEFT, padx=2)

        self.connect_btn = tk.Button(control_frame, text="CONNECT", command=self.toggle_connection, 
                                     bg="#0b1329", fg="#00f3ff", font=("Consolas", 10, "bold"), relief="solid", bd=1)
        self.connect_btn.pack(side=tk.LEFT, padx=10)

        self.mode_btn = tk.Button(control_frame, text="MODE: ROLLING WINDOW", command=self.toggle_time_mode,
                                  bg="#0b1329", fg="#00f3ff", font=("Consolas", 10, "bold"), relief="solid", bd=1)
        self.mode_btn.pack(side=tk.LEFT, padx=10)

        self.record_btn = tk.Button(control_frame, text="🔴 START RECORDING", command=self.toggle_recording, 
                                    bg="#0b1329", fg="#ff0055", activebackground="#ff0055", activeforeground="#050811",
                                    font=("Consolas", 10, "bold"), relief="solid", bd=1)
        self.record_btn.pack(side=tk.LEFT, padx=10)

        tele_frame = ttk.Frame(self.root, padding=5)
        tele_frame.pack(side=tk.TOP, fill=tk.X, padx=10)

        self.telemetry_var = tk.StringVar(value="V: 0.0000V | I: 0.00 uA | P: 0.00 uW | LUX: 0.0 lx | INT: 0 ms | MAX ADC: 0 [N/A]")
        self.tele_lbl = ttk.Label(tele_frame, textvariable=self.telemetry_var, font=("Consolas", 11, "bold"), foreground="#00ff66")
        self.tele_lbl.pack(side=tk.LEFT, padx=5)

        plot_frame = ttk.Frame(self.root, padding=5)
        plot_frame.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=10, pady=5)

        plt.style.use('dark_background')
        self.fig, (self.ax_spec, self.ax_mppt) = plt.subplots(2, 1, figsize=(12, 7))
        self.fig.patch.set_facecolor('#050811')
        self.fig.tight_layout(pad=3.5)

        self.ax_spec.set_facecolor('#080e1a')
        (self.spec_line,) = self.ax_spec.plot(self.wavelengths, self.spectrum, color='#00f3ff', linewidth=1.5)
        self.spec_fill = self.ax_spec.fill_between(self.wavelengths, self.spectrum, color='#00f3ff', alpha=0.15)
        
        self.ax_spec.set_title("C12880MA SPECTRAL INTENSITY PROFILE [WAVELENGTH CALIBRATED]", color='#00f3ff', fontsize=11, family='Consolas')
        self.ax_spec.set_xlim(float(self.wavelengths[0]), float(self.wavelengths[-1]))
        self.ax_spec.set_ylim(0, 4095)
        self.ax_spec.set_xlabel("WAVELENGTH [nm]", color='#00f3ff', family='Consolas')
        self.ax_spec.set_ylabel("ADC COUNTS", color='#00f3ff', family='Consolas')
        self.ax_spec.tick_params(colors='#00f3ff', labelsize=8)
        self.ax_spec.grid(color='#112240', linestyle='--', linewidth=0.5)

        self.ax_mppt.set_facecolor('#080e1a')
        self.ax_mppt_v = self.ax_mppt.twinx()
        self.ax_mppt_v.set_facecolor('none')

        (self.line_v,) = self.ax_mppt_v.plot([], [], color='#00f3ff', label="Vmpp [V]")
        (self.line_i,) = self.ax_mppt.plot([], [], color='#ff0055', label="Impp [uA]")
        (self.line_p,) = self.ax_mppt.plot([], [], color='#00ff66', label="Pmpp [uW]")

        self.ax_mppt.set_title("ESP32 MPPT PERFORMANCE METRICS VS TIME", color='#ff0055', fontsize=11, family='Consolas')
        self.ax_mppt.set_ylabel("CURRENT [uA] / POWER [uW]", color='#ff0055', fontsize=9, family='Consolas')
        self.ax_mppt_v.set_ylabel("VOLTAGE [V]", color='#00f3ff', fontsize=9, family='Consolas')

        self.ax_mppt.tick_params(colors='#ff0055', labelsize=8)
        self.ax_mppt_v.tick_params(colors='#00f3ff', labelsize=8)
        self.ax_mppt.grid(color='#112240', linestyle='--', linewidth=0.5)
        
        lines = [self.line_v, self.line_i, self.line_p]
        labels = [l.get_label() for l in lines]
        self.ax_mppt.legend(lines, labels, loc="upper left", facecolor='#050811', edgecolor='#ff0055', labelcolor='white')

        self.canvas = FigureCanvasTkAgg(self.fig, master=plot_frame)
        self.canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)

    def refresh_ports(self):
        ports = [port.device for port in serial.tools.list_ports.comports()]
        self.port_combobox['values'] = ports
        if ports:
            self.port_combobox.current(0)

    def toggle_connection(self):
        if not self.is_connected:
            selected_port = self.port_combobox.get()
            if not selected_port:
                return
            try:
                self.ser = serial.Serial()
                self.ser.port = selected_port
                self.ser.baudrate = 115200
                self.ser.timeout = 1
                self.ser.dsrdtr = False
                self.ser.rtscts = False
                self.ser.dtr = False
                self.ser.rts = False
                self.ser.open()
                
                self.is_connected = True
                self.connect_btn.config(text="DISCONNECT", fg="#ff0055")
                self.status_lbl.config(text=f"STATUS: ONLINE ({selected_port})", foreground="#00ff66")
            except Exception as e:
                self.status_lbl.config(text=f"STATUS: CONNECTION FAILED", foreground="#ff0055")
                print(f"Connection error: {e}")
        else:
            self.is_connected = False
            if self.ser and self.ser.is_open:
                self.ser.close()
            self.connect_btn.config(text="CONNECT", fg="#00f3ff")
            self.status_lbl.config(text="STATUS: OFFLINE", foreground="#ff0055")

    def toggle_time_mode(self):
        if self.time_mode == "rolling":
            self.time_mode = "compressed"
            self.mode_btn.config(text="MODE: FULL COMPRESSION (t=0)", fg="#00ff66")
        else:
            self.time_mode = "rolling"
            self.mode_btn.config(text="MODE: ROLLING WINDOW", fg="#00f3ff")

    def toggle_recording(self):
        if not self.is_recording:
            filename = f"sensor_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
            filepath = filedialog.asksaveasfilename(defaultextension=".csv", initialfile=filename,
                                                    filetypes=[("CSV Files", "*.csv"), ("All Files", "*.*")])
            if filepath:
                try:
                    self.csv_file = open(filepath, mode='w', newline='')
                    self.csv_writer = csv.writer(self.csv_file)
                    
                    header = ["RecordType", "Timestamp", "Voltage_V", "Current_A", "Power_W", "DutyCycle", 
                              "Temp_C", "Pressure_hPa", "Humidity_perc", "Gas_Ohm", "Lux", "IntegrationTime_us", "Max_ADC", "Saturation_Status"]
                    for wl in self.wavelengths:
                        header.append(f"{wl:.2f}nm")
                    self.csv_writer.writerow(header)
                    
                    self.is_recording = True
                    self.record_btn.config(text="⏹ STOP RECORDING", bg="#ff0055", fg="#050811")
                except Exception as e:
                    print(f"Error opening CSV file: {e}")
        else:
            self.is_recording = False
            if self.csv_file:
                self.csv_file.close()
            self.record_btn.config(text="🔴 START RECORDING", bg="#0b1329", fg="#ff0055")

    def read_serial_loop(self):
        while self.running:
            try:
                if self.is_connected and self.ser and self.ser.is_open:
                    if self.ser.in_waiting:
                        line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                        if line.startswith("LIVE") or line.startswith("SWEEP_POINT"):
                            parts = line.split(',')
                            if len(parts) >= 302:
                                timestamp = parts[1].strip('"')
                                voltage = float(parts[2])
                                current = float(parts[3]) * 1000000.0  # Convert A to uA
                                power = float(parts[4]) * 1000000.0    # Convert W to uW
                                lux = float(parts[10])
                                int_time = int(parts[11])
                                max_adc = int(parts[12])
                                sat_status = parts[13].strip()
                                
                                spec_values = [float(val) for val in parts[14:302]]
                                data_queue.put((timestamp, voltage, current, power, lux, int_time, max_adc, sat_status, spec_values))
                                
                                if self.is_recording and self.csv_writer:
                                    self.csv_writer.writerow(parts)
                else:
                    time.sleep(0.1)
            except Exception as e:
                print(f"Stream parser error: {e}")
                time.sleep(0.05)

    def update_dashboard(self):
        try:
            while not data_queue.empty():
                timestamp, v, i_ua, p_uw, lux, int_time, max_adc, sat_status, spec = data_queue.get_nowait()
                
                self.spectrum = np.array(spec)
                self.spec_line.set_xdata(self.wavelengths)
                self.spec_line.set_ydata(self.spectrum)
                
                self.spec_fill.remove()
                self.spec_fill = self.ax_spec.fill_between(self.wavelengths, self.spectrum, color='#00f3ff', alpha=0.15)
                
                self.ax_spec.set_xlim(float(self.wavelengths[0]), float(self.wavelengths[-1]))
                self.ax_spec.set_ylim(0, max(4095, np.max(self.spectrum) * 1.1))

                int_ms = int_time / 1000.0
                self.telemetry_var.set(
                    f"Vmpp: {v:.4f}V | Impp: {i_ua:.0f} uA | Pmpp: {p_uw:.1f} uW | LUX: {lux:.1f} lx | INT: {int_ms:.1f} ms | MAX ADC: {max_adc} [{sat_status}]"
                )

                if sat_status == "SATURATED":
                    self.tele_lbl.config(foreground="#ff0055")
                else:
                    self.tele_lbl.config(foreground="#00ff66")

                t_label = timestamp.split(' ')[-1] if ' ' in timestamp else timestamp
                self.timestamps.append(t_label)
                self.voltage_history.append(v)
                self.current_history.append(i_ua)
                self.power_history.append(p_uw)

                if self.time_mode == "rolling" and len(self.timestamps) > 60:
                    self.timestamps.pop(0)
                    self.voltage_history.pop(0)
                    self.current_history.pop(0)
                    self.power_history.pop(0)

                x_indexes = np.arange(len(self.timestamps))
                
                self.line_v.set_xdata(x_indexes)
                self.line_v.set_ydata(self.voltage_history)
                
                self.line_i.set_xdata(x_indexes)
                self.line_i.set_ydata(self.current_history)

                self.line_p.set_xdata(x_indexes)
                self.line_p.set_ydata(self.power_history)

                self.ax_mppt.set_xlim(0, max(len(x_indexes) - 1, 1))
                self.ax_mppt_v.set_xlim(0, max(len(x_indexes) - 1, 1))

                if self.current_history or self.power_history:
                    max_left = max(max(self.current_history or [0]), max(self.power_history or [0]))
                    self.ax_mppt.set_ylim(0, max(max_left * 1.2, 10.0))

                if self.voltage_history:
                    max_v = max(self.voltage_history)
                    min_v = min(self.voltage_history)
                    span = max_v - min_v
                    buffer = max(span * 0.2, 0.2)
                    self.ax_mppt_v.set_ylim(max(0, min_v - buffer), max_v + buffer)
                
                if len(x_indexes) > 0:
                    step = max(1, len(x_indexes) // 6)
                    self.ax_mppt.set_xticks(x_indexes[::step])
                    self.ax_mppt.set_xticklabels(self.timestamps[::step], rotation=15, color='#ff0055', fontsize=8)

                self.canvas.draw()
        except Exception as e:
            print(f"UI Render error: {e}")

        self.root.after(30, self.update_dashboard)

    def on_closing(self):
        self.running = False
        if self.is_recording and self.csv_file:
            self.csv_file.close()
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = SciFiDashboard(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()