# AS7341 Mesh Node (spettrometro)

Nodo Bluetooth Mesh su Seeed XIAO nRF52840 Sense con sensore spettrale
AS7341 (11 canali, I2C `0x39`). Espone illuminamento (lux), temperatura di
colore (K), cromaticità CIE 1931 e lo spettro (tipo custom `0xF341`) da un
Sensor Server. Stessa struttura del nodo [SCD41](https://github.com/adelpra2527vi4/CO2_Mesh).

## Hardware

- Seeed XIAO nRF52840 Sense (`xiao_ble`)
- AS7341 su D4/D5 (default scheda) oppure D9/D10 (P1.14 SDA / P1.15 SCL): il firmware prova entrambi all'avvio

## Firmware

- nRF Connect SDK v3.3.1 (Zephyr); Zephyr non ha un driver AS7341, l'accesso ai registri è in `src/main.c`
- Company ID `0x0059`, nome in advertising `AS7341 Light`
- Dettagli su modelli e sensori mesh in [README-mesh.md](README-mesh.md); calibrazione lux in [CALIBRAZIONE-LUX.md](CALIBRAZIONE-LUX.md)

## Build

```bash
./build.sh
```

## Shell USB (CDC)

| Comando | Funzione |
|---|---|
| `node status` | stato del nodo e ultima misura |
| `node reset` | reset mesh locale (torna provisionabile) |
| `node dfu` | riavvia nel bootloader per aggiornare il firmware |

## Struttura

- `src/main.c`: applicazione
- `prj.conf`: configurazione Zephyr/Bluetooth Mesh
- `boards/xiao_ble.overlay`: pin I2C
