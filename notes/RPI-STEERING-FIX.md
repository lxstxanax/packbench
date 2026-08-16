# Fix: wheels sit at full left lock, one side dead

**One line to change on the Raspberry Pi.** Everything else here is why.

---

## The file

```
/opt/auto_controller/controller.py
```

⚠️ **Not** `~/controller.py` and **not** `~/auto_controler/controller.py`. Both exist and
neither is the one that runs. The live service runs the `/opt` copy — confirmed with
`ps aux`:

```
slycro  730  /opt/auto_controller/.venv/bin/python /opt/auto_controller/controller.py
```

Service name: **`racecar`** (`systemctl restart racecar`).

---

## The change

Function `MotorController._steer_to_wire`, around line 517.

**Now:**

```python
    def _steer_to_wire(self, value: int) -> int:
        """Pi-intern 0..100 (50 = Mitte) -> Protokoll -100..100 (0 = Mitte)."""
        return max(-100, min(100, (value - STEER_CENTER) * 2))
```

**Must be:**

```python
    def _steer_to_wire(self, value: int) -> int:
        """Pi-intern 0..100 (50 = Mitte) -> Protokoll 0..100 (50 = Mitte)."""
        return max(0, min(100, int(value)))
```

Then:

```bash
sudo systemctl restart racecar
```

---

## Why

The STM32 expects the STEER byte as **unsigned 0..100, where 50 is centre**. That is what
`car_fw/Core/Src/steering.c` implements and it has not changed:

```c
// 0..50   maps LEFT  -> CENTER      i.e. 0   = full LEFT   (2400 us)
// 50..100 maps CENTER -> RIGHT      i.e. 100 = full RIGHT  ( 800 us)
```

`_steer_to_wire` converts to a **signed** range instead, centre = 0. So:

| Pi means | sends | STM32 reads it as |
|---|---|---|
| centre (50) | **0** | **full LEFT lock** |
| full right (100) | +100 | full right |
| full left (0) | −100 | byte 156 → **out of range, packet dropped** |

That is exactly the observed behaviour: wheels at full left when the stick is untouched,
moving the stick brings them *towards* centre, and one direction does nothing at all.

## Measured on the STM32, live

The firmware counts what actually arrives (console key `l`):

```
CONTROL ok  4529     range bad  77        <- and climbing
last CONTROL payload: motor 0 %   steer 0 <- 0 at rest; should be 50
seen so far: |motor| max 0 %   steer 0..100
```

`range bad` counts packets rejected because a value was out of 0..100 — those are the
negative steering values. It grows whenever the stick is pushed to that side.

## This is a regression from today

The `/opt` file is dated **Aug 16 13:35**. Earlier the same day, with the same STM32
firmware, the link read `steer 50` at rest and the car drove correctly from the joystick.
The STM32 side did not change its steering protocol at any point.

The stale class docstring at line ~445 still says `STEER = signed int8 (-max_steer..+max_steer)`
— that is the obsolete description the new code was written against. It should say
`STEER = unsigned 0..100, 50 = Mitte`.

---

## Verify in 30 seconds, car stationary

Press `l` on the STM32 console:

1. Stick untouched → `last CONTROL payload: ... steer 50` (**50**, not 0).
2. Sweep the stick slowly → `steer` shows **intermediate** values (23, 41, 67…).
3. `range bad` stops climbing.

If step 1 still shows 0, the running file was not the one edited, or `racecar` was not
restarted.

---

## One more thing, not urgent

`MotorController.stop()` hardcodes the steer byte to `0` in a CONTROL packet. Under the
correct protocol `0` is **full left lock**, not centre. It appears to be unused today, but
if anything ever calls it the wheels will slam over. It should send `STEER_CENTER` (50).
