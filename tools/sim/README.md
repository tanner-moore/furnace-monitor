# Furnace simulator

Runs the web page in `data/index.html` on a laptop with made-up furnace data, so the page can be tried before the CO16 is wired in.

```sh
node tools/sim/server.mjs --speed 5   # then open http://localhost:8080/
```

Needs Node 18 or later and `g++`. The server compiles `sim.cpp` together with the real furnace model in `lib/furnace_core`, simulates the last 24 hours for the History and Events tabs, then plays one of everything live: a two-stage heat cycle (W1, then W2 after 3 minutes), post-purge and blower overrun, fan only, a pressure switch fault (code 3) and an ignition lockout (code 2). After that it settles into ordinary heat cycles.

`--speed` runs the furnace up to 50 times faster than real time. `--seed` picks a different day. Settings and firmware uploads are accepted and ignored.

The sequence timings, currents and temperatures are plausible guesses for a White-Rodgers 50A51 on a Trane TUD120, not measurements. Replace them with real readings once the board is on the furnace.
