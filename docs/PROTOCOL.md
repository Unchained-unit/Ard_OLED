# NMSE OLEDer RS-485 protocol

Serial settings:

```text
9600 baud, 8N1
```

All commands:

```text
board_id,COMMAND,args...
```

Examples:

```text
1,PING
2,STATUS
1,READ
1,READ,3
2,SET,4,0.800000,0.200000
1,PHGAIN,3,16
2,ZERO
```

Responses:

```text
board_id,OK,PONG
board_id,STATUS,ADS=1,DAC=1,PHGAIN=2/3;2/3;2/3;2/3,MS=12345
board_id,DATA,sample,millis,Uraw_V,Iraw_V,PhotoRaw_V,Uapprox_V,Iapprox_mA
board_id,END
board_id,ERR,...
```

The PC is always the bus master. Arduino boards only answer addressed commands and stay silent for commands addressed to other boards.

For current firmware, `Uapprox_V` and `Iapprox_mA` are the authoritative
physical measurements already converted by Arduino. The PC application must
not multiply `Uraw_V` or `Iraw_V` by the hardware scale a second time.
`Uraw_V`, `Iraw_V`, and `PhotoRaw_V` remain available for diagnostics and
calibration. If an old firmware response ends after `PhotoRaw_V`, the PC falls
back to its legacy raw-value conversion.

Photo ADS1115 gain command:

```text
board_id,PHGAIN,sample,gain
```

Current/shunt ADS1115 gain command:

```text
board_id,ICGAIN,sample,gain
```

Example: `1,ICGAIN,2,8` switches the current measurement of sample 2 on board 1 to the ±0.512 V PGA range. Accepted gain values are `2/3`, `1`, `2`, `4`, `8`, and `16`.

Accepted gain values:

```text
2/3, 1, 2, 4, 8, 16
```

Ranges:

```text
2/3 -> 6.144 V
1   -> 4.096 V
2   -> 2.048 V
4   -> 1.024 V
8   -> 0.512 V
16  -> 0.256 V
```
