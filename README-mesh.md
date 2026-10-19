# Nodo mesh AS7341 (luce e colore)

Nodo Bluetooth Mesh su Seeed XIAO nRF52840 Sense con sensore spettrale AS7341.
Espone **illuminamento** e **temperatura di colore** tramite un Sensor Server
standard. Stessa struttura del nodo SCD41 (CO2/T/RH): un'app che legge uno sa
leggere anche l'altro.

## Modelli mesh

Un solo elemento (indirizzo primario del nodo) con:

| Modello | Note |
|---|---|
| Configuration Server | |
| Health Server | l'attention (identify) fa lampeggiare 4 volte in bianco il LED |
| Sensor Server | 3 sensori, vedi sotto |
| Remote Provisioning Server | il nodo fa da ponte per provisionare altri nodi |

Company ID: `0x0059`. Nome in advertising: `AS7341 Light`.

## Sensori

Tutti sono sullo stesso Sensor Server, quindi un solo *Sensor Descriptor Get*
li scopre tutti.

| Property ID | Nome | Formato | Unita' | Risoluzione |
|---|---|---|---|---|
| `0x0055` | Present Illuminance | 24 bit unsigned | lux | 0,01 lx |
| `0x0051` | Present Correlated Color Temperature | 16 bit unsigned | kelvin | 1 K |
| `0xF341` | **Spettro (tipo personalizzato, non standard)** | 10 canali, ognuno 16 bit unsigned (formato Count 16) | conteggi normalizzati | 0,01 |

### Spettro (`0xF341`)

Il Property ID non e' definito dalla specifica Bluetooth: e' scelto dal firmware.
Un'app che non lo conosce lo vedra' come sensore sconosciuto.

I 10 canali sono, in ordine (ognuno uint16 little-endian):

| # | Canale | Lunghezza d'onda |
|---|---|---|
| 0-7 | F1, F2, F3, F4, F5, F6, F7, F8 | 415, 445, 480, 515, 555, 590, 630, 680 nm |
| 8 | Clear | banda larga visibile |
| 9 | NIR | vicino infrarosso |

Valore del canale = `conteggi / (guadagno * tempo_integrazione_ms) * 100`,
saturato a 65534. Dividere per 100 per avere i conteggi normalizzati: cosi' le
letture sono confrontabili anche se il guadagno automatico cambia.
Con luce molto forte e guadagno basso alcuni canali possono saturare a 65534.
`0xFFFF` significa "non disponibile".

- Usare *Sensor Get* (opcode `0x8231`) con il Property ID per leggere un valore.
  Se non c'e' ancora nessuna misura valida il nodo risponde con errore (`-EAGAIN`
  lato firmware): riprovare dopo qualche secondo dal boot.
- Il tipo del Sensor Get e' quello standard: nessun modello vendor.

## Pubblicazione

- Misura ogni **5 s**.
- Pubblica (Sensor Status) ogni **30 s**, se il nodo e' provisionato e la
  publication del Sensor Server e' configurata (altrimenti non pubblica nulla:
  e' normale).
- Per ricevere i valori in push, il provisioner deve impostare Publish Address
  e AppKey sul Sensor Server e fare il bind dell'AppKey.

## Attendibilita' dei valori

**I valori sono indicativi, non tarati.**

- **Lux**: stimati dai canali F1-F8 pesati con la curva CIE `ybar`, poi moltiplicati
  per una costante provvisoria (`LUX_K` in `src/main.c`). Finche' non viene
  tarata con un luxmetro il valore puo' essere molto lontano dal reale
  (con luce ambiente normale oggi risulta ~0).
- **Temperatura di colore**: calcolata da X/Y/Z stimati dagli stessi canali con
  la formula di McCamy. Dipende dal rapporto fra i canali, quindi e' meno
  sensibile alla costante dei lux, ma resta approssimata. Limitata a 800-65000 K.
- Le app possono mostrare i valori, ma non dovrebbero usarli per decisioni che
  richiedono misure precise.

## Non esposti via mesh

I conteggi grezzi non normalizzati e il guadagno non sono pubblicati.
Si leggono solo dalla shell USB (`node status`).

## Provisioning e reset

- Il nodo e' provisionabile via **PB-ADV** e **PB-GATT**. UUID derivato dal
  Device ID hardware (stabile fra i riavvii).
- Non provisionato: LED blu, lampeggio breve ogni secondo.
  Link aperto: blu fisso. Provisionato: verde, impulso ogni 3 s.
- **Node Reset** dalla mesh (Config Client): il nodo lampeggia rosso per 2 s e
  torna provisionabile da solo.
- Identify / attention (Health Server o attention dell'invito di provisioning):
  4 lampi bianchi, pausa, ripete finche' l'attention e' attiva.

## Shell USB (CDC, 115200)

| Comando | Effetto |
|---|---|
| `node status` | stato mesh, indirizzo, lux, K, guadagno, F1-F8, clear, NIR |
| `node reset` | reset mesh locale (torna provisionabile) |
| `node dfu` | riavvia nel bootloader per l'aggiornamento firmware |

## Hardware

- Seeed XIAO nRF52840 Sense, AS7341 su I2C indirizzo `0x39`.
- Il firmware prova prima D4/D5 e poi D9/D10 (P1.14 SDA / P1.15 SCL).
  Al momento il sensore e' collegato su **D9/D10**.
- Il tempo di integrazione e' ~50 ms; il guadagno si regola da solo
  (0,5x-512x) sul canale clear.
