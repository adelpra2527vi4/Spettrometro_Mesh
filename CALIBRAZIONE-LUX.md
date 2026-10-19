# Calibrazione del sensore di luce via mesh (metodo Silvair)

L'app MeshProv/GDSMesh non corregge il dato lato app: scrive il **lux reale**
dentro il sensore con un *Sensor Setting Set*. Da quel momento il sensore
pubblica valori gia' corretti, quindi li vedono tutte le app e le lampade.

Riferimento app: `calibrateAmbientLightSensor` e `readAmbientLightCalibration`
in `MeshRepository.kt`.

## Messaggio di scrittura

- **Opcode:** Sensor Setting Set `0x59` (senza conferma: `0x5A`).
- **Destinazione:** l'elemento con il Sensor Setup Server (modello `0x1101`),
  cifrato con l'AppKey.
- **Payload:** 7 byte, tutti little-endian.

| Byte | Contenuto |
|---|---|
| 0-1 | Sensor Property ID = `0x004E` (Present Ambient Light Level) |
| 2-3 | Sensor Setting Property ID = `0x004E` (uguale al precedente) |
| 4-6 | Valore: uint24 in **centilux** (lux x 100). `0xFFFFFF` = "non noto" |

Esempio: 320,50 lux = `32050` = `0x007D32` -> byte `32 7D 00`.

## Risposta attesa

Sensor Setting Status `0x5B`:

| Byte | Contenuto |
|---|---|
| 0-1 | Sensor Property ID |
| 2-3 | Sensor Setting Property ID |
| 4 | Access: 1 = sola lettura, 2 = sola scrittura, 3 = lettura e scrittura |
| 5-7 | Valore memorizzato (uint24, centilux) |

L'app controlla che il valore tornato sia quello scritto (3 tentativi, timeout
2 s). Se la setting manca o il valore e' vuoto, la considera non supportata.

## Lettura e scoperta

| Messaggio | Opcode | Uso |
|---|---|---|
| Sensor Settings Get | `0x8235` | Lista delle setting della property `0x004E`. Risposta: Sensor Settings Status `0x58`. Se `0x004E` non e' in lista, la calibrazione risulta non supportata |
| Sensor Setting Get | `0x8236` | Rilegge il valore memorizzato (owner `0x004E`, setting `0x004E`). Risposta: `0x5B` |

## Cosa deve fare il firmware

1. Esporre la setting sul sensore da calibrare, con Access = lettura e scrittura (3).
2. Alla ricezione del valore reale R: coefficiente = R / misura grezza attuale.
   Applicarlo a tutte le letture successive.
3. Salvare il coefficiente in flash (settings) perche' sopravviva al riavvio.
4. Rispondere con lo status contenente il valore memorizzato.

## Attenzione al Property ID

L'app calibra solo la `0x004E`, cioe' il sensore lux classico. L'AS7341 espone
l'illuminamento come `0x0055` (Present Illuminance). Due strade:

- **Setting sulla `0x0055`:** il nodo la espone con owner `0x0055` e l'app va
  adattata per scrivere li' quando il nodo e' un AS7341.
- **Setting sulla `0x004E`:** il nodo la espone sulla `0x004E` anche se il valore
  misurato e' sulla `0x0055`. L'app funziona senza modifiche.

## Nota sui lux a zero

Con `LUX_K` provvisorio l'AS7341 riporta 0 lux con luce normale, e il Sensor
Status per `0x0055` esce vuoto (solo Property ID, nessun valore): l'app non ha
nulla da mostrare. La calibrazione risolve il valore, ma conviene verificare
anche perche' lo zero non viene codificato.
