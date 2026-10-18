# Original User Request

## 2026-10-18T14:08:34Z

Receptor VBAN (VB-Audio Network) de máxima calidad audiófila y baja latencia para Android, implementado con C++20 (NDK), Oboe, procesamiento Float32 hasta 768 kHz, búfer circular lock-free SPSC, compensación adaptativa de reloj (clock drift), bypass de mezclador USB DAC en Android 14+ y UI reactiva en Jetpack Compose con servicio en primer plano.

Working directory: D:\Workspaces\vban-receptor
Integrity mode: development

## Requirements

### R1. Núcleo de Red y Procesamiento Nativo en C++ (NDK + Oboe)
Recepción UDP de flujos VBAN-Audio con búfer de socket de 4 MB, decodificación de formatos PCM (16/24/32 bits) y Float32 a 32-bit Float normalizado, canalización hacia un búfer circular Lock-Free SPSC y reproducción con Oboe en modo exclusivo de baja latencia hasta 768 kHz.

### R2. Compensación Dinámica de Deriva de Reloj (Clock Drift) y Reconfiguración de Flujo
Alineación temporal continua mediante remuestreo adaptativo con micro-interpolación para prevenir underrun/overflow por desfase de relojes de cuarzo sin artefactos audibles, junto con reconfiguración automática del flujo de audio ante cambios de frecuencia de muestreo o canales.

### R3. Servicio en Primer Plano, Gestión Energética y Soporte Bit-Perfect
Servicio en primer plano con tipo `mediaPlayback`, adquisición de `WifiLock` en modo de baja latencia (`WIFI_MODE_FULL_LOW_LATENCY`) y `WakeLock`, soporte de recepción Multicast, y enrutamiento hacia DACs USB externos con bypass del mezclador del sistema (`MIXER_BEHAVIOR_BIT_PERFECT`) en Android 14+.

### R4. Interfaz de Usuario y Telemetría en Tiempo Real (Jetpack Compose)
Dashboard moderno en Jetpack Compose con control de conexión, selección de dispositivo de audio, indicadores estéreo de nivel (VU Meter), medidor de jitter/ocupación del búfer, frecuencia de muestreo detectada y contador de paquetes perdidos.

### R5. Entorno de Construcción y SDK
Configurar el proyecto Gradle con CMake 3.22+, NDK (usando la ruta del SDK `D:\Android\sdk`), Oboe 1.9.0 vía Prefab y dependencias de Jetpack Compose.

## Acceptance Criteria

### Audio & Network Engine
- [ ] Compilación nativa C++20 con CMake y enlazado de Oboe 1.9.0 sin errores ni advertencias críticas.
- [ ] Parser conforme a la especificación oficial VBAN (cabecera de 28 bytes, FourCC 'VBAN', subprotocolo audio, tabla de 21 tasas oficiales).
- [ ] Desempaquetado correcto de PCM 16-bit, 24-bit little-endian empaquetado, 32-bit int y 32-bit float a Float32 normalizado `[-1.0f, +1.0f]`.
- [ ] Búfer circular SPSC libre de bloqueos y asignaciones en el callback de audio en tiempo real.
- [ ] Compensación de deriva de reloj que mantenga la estabilidad del búfer en sesiones prolongadas.

### Android System & Lifecycle
- [ ] Servicio en primer plano persistente con controles y metadata en notificación del sistema.
- [ ] Prevención de cortes de red con pantalla apagada mediante `WifiLock` (`WIFI_MODE_FULL_LOW_LATENCY`) y `WakeLock`.
- [ ] Reconocimiento y enrutamiento a USB DACs externos con soporte para modo exclusivo / bit-perfect en Android 14+.

### Verification Suite
- [ ] Suite de pruebas unitarias ejecutables en C++ que verifique:
  - Validación de cabecera VBAN con paquetes de prueba.
  - Conversión y normalización de formatos de audio (16-bit, 24-bit empaquetado, 32-bit float).
  - Integridad y rendimiento del búfer SPSC bajo concurrencia.
- [ ] Compilación exitosa del APK mediante `./gradlew assembleDebug` (o `gradlew.bat assembleDebug`) generando el paquete instalable.


## Follow-up — 2026-10-18T15:43:45Z

USER DIRECTIVE: Please pause all orchestration immediately upon completing and gating Milestone 3. Once Milestone 3 verification is finished, do NOT launch Milestone 4. Report the completion of Milestone 3 and wait for user approval before resuming.


## Follow-up — 2026-10-18T16:21:49Z

USER APPROVAL: The user has authorized continuing with Milestone 4. Please resume orchestration immediately, launch Milestone 4 (UDP Network Receiver with 4 MB buffer, multicast support, and JNI Telemetry Bridge), and proceed with implementation and verification.
