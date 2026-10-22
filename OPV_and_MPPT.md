# Indoor Ambient Light Energy Harvesting and Spectral Intelligence

The future of energy harvesting hinges on the synergy between advanced materials and intelligent monitoring, a domain perfectly embodied by **Organic Photovoltaics (OPVs)** and novel PV technologies deployed in indoor environments.

---

## Why Indoor Photovoltaics?

Advanced PV cells offer distinct advantages over rigid silicon panels:

- **Lightweight**
- **Mechanically flexible**
- **Tunable light absorption spectra**

This unique trait allows researchers to design cells that both:

1. Generate electricity from ambient sources  
2. Characterise the ambient light spectrum  

---

## The Challenge: Material Degradation

A significant deployment challenge is **degradation over time**. Photovoltaic materials suffer from complex deterioration pathways caused by:

- Photo-oxidation  
- Thermal stress  
- Environmental exposure  

This leads to a measurable decline in performance. Effectively combating and modelling this degradation requires **abundant, synchronized, high-fidelity data**—precisely the goal of this IoT sensing platform.

---

## Maximum Power Point Tracking (MPPT) and Firmware Optimisation

To ensure every photon is maximised despite fluctuating indoor illumination:

- **MPPT is essential**.  
- The embedded **ESP32-based design** implements a customised **Perturb and Observe (P&O)** algorithm running at a 1-second interval (`MPPT_INTERVAL_MS = 1000`).
- To prevent instability from electrical noise or minor environmental ripples, the firmware applies **exponential moving average smoothing** (`alpha = 0.4`) to raw voltage, current, and power measurements:
  $$\begin{aligned} V_{\text{new}} &= 0.4 V_{\text{raw}} + 0.6 V_{\text{old}} \\ I_{\text{new}} &= 0.4 I_{\text{raw}} + 0.6 I_{\text{old}} \\ P_{\text{new}} &= 0.4 P_{\text{raw}} + 0.6 P_{\text{old}} \end{aligned}$$
- **Power Deadband & Locking Mechanism:** When power variations fall within a tight deadband ($\Delta P < 1.5\,\mu\text{W}$), a lock counter engages (`lock_counter > 3`), **holding** the duty cycle stable to prevent unnecessary hunting around the maximum power point.
- **Fine-Step Adjustments:** When active, the system adjusts the PWM duty cycle via a controlled fine step (`FINE_STEP = 10`) across a 12-bit resolution range ($0\text{--}4095$).

### Low-Light Fix

A key adaptation in the **ESP32-based design** is the **low-light fix**:

- **Night Parking:** Automatically pauses the P&O routine and parks the MOSFET load at a safe duty cycle (`DUTY_PARK = 50`) when ambient light drops below the threshold ($\text{Lux} \le 50$).
- **Daybreak Reset:** Resets state variables (`V_old`, `I_old`, `P_old`) and re-engages tracking when light returns, preventing inefficient operation and protecting the PV module.

---

# Spectral Dynamics and Indoor Environment Monitoring

The ambient light environment in indoor spaces or controlled testbeds is **highly dynamic** - its measurement is crucial for maximising **both energy efficiency and harvesting performance**.

---

# Data Acquisition and Spectral Intelligence: The IoT-Spectra Connection

Here is where the integrated sensing platform shows its true innovation:

The **ambient light** is both:

- The **energy source** for the PV array  
- The **signal** analysed by the **C12880MA MEMS Spectrometer**  

The platform, built around the **ESP32 microcontroller** and **INA226 power sensor**, allows for the simultaneous acquisition of critical data:

- Logging the **electrical performance** of the PV module under real-world indoor conditions  
- Capturing a **spectral fingerprint** of the ambient light via on-chip 5th-order polynomial nanometre conversion (288 channels)  

This high-fidelity data provides the foundation for researchers to **tune photovoltaic designs**—optimising their spectral transparency and harvesting efficiency for specific indoor lighting spectra.

---

## Towards Smart, Self-Sustaining Systems

By continuously logging synchronised **electrical, spectral, and environmental data** (utilising SD card wear-levelled 30-second batch buffering), the platform builds training datasets for advanced **ML/DL models**.  

These models enable:

- **Predictive modelling** of material aging  
- **Actionable insights** for power management  

Ultimately transforming ambient light monitoring into **smart, self-sustaining energy systems**.

