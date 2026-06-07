<img src="https://capsule-render.vercel.app/api?type=waving&color=0:10b981,100:09090b&height=120&section=header" width="100%">

<div align="center">
  <img src="img/manual-and-schedule.png" alt="OmniRelay Logo" width="100%" style="border-radius: 12px; border: 1px solid #27272a;">
  
  <br />
  <br />

  # 🔌 OMNIRELAY ESP8266
  <a href="https://github.com/HyLuthfi"><img src="https://readme-typing-svg.demolab.com?font=Inter&weight=800&size=22&pause=1000&color=10B981&center=true&vCenter=true&width=800&lines=Advanced+4-Channel+IoT+Relay+System;Real-Time+WebSocket+Control;Standalone+Web+Server+with+Dark+UI;RTC-Powered+Automated+Scheduling" alt="Typing SVG" /></a>

  <p align="center">
    <img src="https://img.shields.io/badge/C%2B%2B-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white" alt="C++">
    <img src="https://img.shields.io/badge/ESP8266-000000?style=for-the-badge&logo=espressif&logoColor=white" alt="ESP8266">
    <img src="https://img.shields.io/badge/HTML5-E34F26?style=for-the-badge&logo=html5&logoColor=white" alt="HTML5">
    <img src="https://img.shields.io/badge/CSS3-1572B6?style=for-the-badge&logo=css3&logoColor=white" alt="CSS3">
    <img src="https://img.shields.io/badge/JavaScript-F7DF1E?style=for-the-badge&logo=javascript&logoColor=black" alt="JavaScript">
  </p>

  <p align="center">
    Sebuah sistem otomasi pintar berbasis ESP8266 untuk mengontrol 4 channel relay. Dilengkapi <b>Real-Time Web Dashboard</b>, Penjadwalan RTC DS3231, Timer, dan <b>Premium Minimalist Dark Mode</b> yang tertanam sepenuhnya di dalam firmware.
  </p>
</div>

<p align="center"><img src="https://user-images.githubusercontent.com/73097560/115834477-dbab4500-a447-11eb-908a-139a6edaec5c.gif" width="100%"></p>

## Fitur Utama

<table align="center" width="100%">
  <tr>
    <td width="50%" valign="top">
      <b>Real-Time Sync Dashboard</b><br/>
      Antarmuka web tanpa <i>lag</i> yang tersinkronisasi instan melalui WebSocket. Latar belakang <i>Deep Dark Zinc</i>.
    </td>
    <td width="50%" valign="top">
      <b>4 Intelligent Modes</b><br/>
      Mendukung mode kontrol Manual, Schedule (Jadwal RTC), Timer Mundur, dan Cycle (Loop On/Off).
    </td>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <b>Cinematic Light Show</b><br/>
      Mode <i>testing</i> dengan 19+ pola efek pencahayaan dinamis (Wave, Strobe, Breathing, dll) khusus untuk aktuator lampu.
    </td>
    <td width="50%" valign="top">
      <b>Secure Admin Panel</b><br/>
      Panel rahasia untuk mengubah konfigurasi WiFi AP, pemetaan Pin (D1-D8), dan akses Web Serial Monitor.
    </td>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <b>Standalone Firmware (PROGMEM)</b><br/>
      Seluruh HTML, CSS, dan JS dikompilasi utuh ke dalam C++ tanpa memerlukan file SPIFFS/LittleFS eksternal.
    </td>
    <td width="50%" valign="top">
      <b>OTA Firmware Updates</b><br/>
      Lakukan pembaruan kode <i>firmware</i> <code>.bin</code> langsung melalui browser, tanpa kabel USB.
    </td>
  </tr>
</table>

<p align="center"><img src="https://user-images.githubusercontent.com/73097560/115834477-dbab4500-a447-11eb-908a-139a6edaec5c.gif" width="100%"></p>

## 📸 Galeri Sistem

### 1. Manual & Schedule Control
Manajemen relay harian dengan dukungan sinkronisasi RTC (DS3231) untuk eksekusi tanpa koneksi internet.
<img src="img/manual-and-schedule.png" alt="Manual Schedule" width="100%" style="border-radius: 8px; border: 1px solid #27272a; margin-bottom: 20px;">

### 2. Timer & Cycle Configuration
Mode penghitung waktu otomatis dan siklus berulang, sangat cocok untuk penyiraman tanaman hidroponik atau aerasi akuarium.
<img src="img/timer-and-cycletime.png" alt="Timer Cycle" width="100%" style="border-radius: 8px; border: 1px solid #27272a; margin-bottom: 20px;">

### 3. Admin Panel - System Status & WiFi
Konfigurasi jaringan dan pemantauan memori <i>heap</i> secara terpusat dan aman.
<img src="img/admin-pt1.png" alt="Admin WiFi" width="100%" style="border-radius: 8px; border: 1px solid #27272a; margin-bottom: 20px;">

### 4. Hardware Pin Mapping & Serial Monitor
Ubah pin aktuator secara dinamis langsung dari web, lengkap dengan terminal <i>debugger</i> nirkabel.
<img src="img/admin-pt2.png" alt="Admin Pins" width="100%" style="border-radius: 8px; border: 1px solid #27272a; margin-bottom: 20px;">

<p align="center"><img src="https://user-images.githubusercontent.com/73097560/115834477-dbab4500-a447-11eb-908a-139a6edaec5c.gif" width="100%"></p>

## 🚀 Teknologi yang Digunakan

### Frontend (Embedded in Firmware)
- **Styling:** Vanilla CSS3 dengan skema <i>Premium Minimalist Dark Mode</i>.
- **Typography:** Inter (Sans-serif modern).
- **Interactivity:** Vanilla JavaScript dengan WebSocket API terintegrasi.

### IoT & Firmware (C++ / ESP8266)
- **Microcontroller:** ESP8266 (NodeMCU / Wemos D1 Mini)
- **Language:** C++ (Arduino Framework)
- **Hardware Integration:**
  - 4-Channel Relay Module (Active Low / High)
  - RTC DS3231 Module (I2C: `SDA=D2`, `SCL=D1`)
- **Libraries Used:** 
  - `ESP8266WiFi`, `ESP8266WebServer`, `EEPROM`, `Wire`
  - `WebSocketsServer` by Markus Sattler
  - `RTClib` by Adafruit
  - `ESP8266HTTPUpdateServer`

<p align="center"><img src="https://user-images.githubusercontent.com/73097560/115834477-dbab4500-a447-11eb-908a-139a6edaec5c.gif" width="100%"></p>

## 🛠️ Panduan Instalasi & Flashing

1. **Buka Proyek**
   Buka file `relay_otomatis.ino` menggunakan Arduino IDE (pastikan paket board ESP8266 sudah terinstal).
2. **Konfigurasi Variabel**
   Cari blok `// --- KONFIGURASI PENGGUNA ---` di baris teratas kode. Anda bisa mengubah SSID Access Point bawaan, password admin, atau hostname di sini.
3. **Compile & Upload**
   Sambungkan ESP8266, pilih port USB yang sesuai, lalu *Upload* kode.
4. **Hubungkan & Akses**
   - Hubungkan WiFi laptop/HP ke SSID ESP8266 (Contoh: `ESP8266_Relay`).
   - Buka browser dan akses **`http://192.168.4.1/`** untuk Dashboard Utama.
   - Buka **`http://192.168.4.1/admin`** untuk Admin Panel (User: `admin`, Pass: `admin123`).

<p align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0:10b981,100:09090b&height=120&section=footer" width="100%"/>
</p>
