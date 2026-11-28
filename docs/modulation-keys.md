# Modulation keys: `live_mod`, `mod_map`, `mod_set`

*A draft for Schwung's module docs, asked for in the review of this pull request. Written by Claude, Rainer
Geissendoerfer's coding assistant; every number about Surge below is measured (see the pull request).*

Three optional keys that let a host show and edit a module's own modulation. A module may support all, any
or none of them. A module that does not know a key answers the read as it answers any unknown key, and a
host must cope with that: nothing here is required.

All three follow the rules for `get_param` and `set_param` in `MODULES.md`: a read is served on the audio
thread, so it allocates nothing, takes no lock and does no work that grows with the instrument.

## `get_param("live_mod")`: where the sound has its modulated controls now

One read for every moving value.

```json
{"filter1_cutoff": 71.944763, "feedback": 0.721569}
```

- **Keys**: the module's own parameter keys. Only controls that the module's own modulation (an LFO, an
  envelope, velocity, a macro) is moving now. A control nothing moves is left out.
- **Values**: where the sound has the control at this moment, in the units and form the plain key answers
  in for a number.
- **At rest**: `{}`, when nothing sounds or nothing is routed.
- **It agrees with `:effective`.** A module that answers `live_mod` answers `<key>:effective` with the same
  number from the same reading, and falls back to the control's own value when nothing moves it.
- **Polyphony**: one value per control. Surge reports its newest voice.
- **Freshness**: the values are written while a block renders, so they stand still while a silent slot is
  not rendered.

## `get_param("mod_map")`: what can be routed, and what is

```json
{
  "sources": [{"id": "lfo1", "name": "LFO 1"}, {"id": "velocity", "name": "Velocity"}],
  "dests":   ["filter1_cutoff", "osc1_pitch"],
  "routes":  [{"src": "lfo1", "dst": "filter1_cutoff", "amount": 0.5}]
}
```

- **`sources`**: what a route can start from, in the order a player reaches for them. `id` is what
  `routes` and `mod_set` use; `name` is for a screen. A source may carry its own `"dests"` where it can
  reach only some controls (a synth of fixed routes).
- **`dests`**: the parameter keys a route may reach. Absent: every numeric parameter.
- **`routes`**: every route the sound has. `amount` is -1..1 of the destination's range. A route may carry
  `"bipolar": true|false` where the synth has that choice.
- **`names`** (optional): `{"pitch": "Pitch"}`, for a destination that has no parameter of its own.

## `set_param("mod_set", …)`: one route made, changed or removed

```json
{"src": "lfo1", "dst": "filter1_cutoff", "amount": 0.5}
```

- `amount` 0 removes the route. `"bipolar"` may be given where the synth has that choice.
- An unknown source or destination, or a control that cannot be modulated, is ignored.
- A route set this way shows in the next `mod_map` and is part of the module's `state`, so a preset
  brings it back. A state without routes loads as it always did.

## When the answer does not fit the buffer

Both reads leave **whole JSON** in the buffer: entries that do not fit are left out, never cut.

- `mod_map` then carries `"truncated": true`.
- `live_mod` leaves out the controls that do not fit.
- The read returns the length of the whole answer, which is then at least `buf_len` (as `snprintf`
  would), so a host that can ask again with more room knows to.

## Cost, as measured for Surge XT

181 destinations, 40 sources, a map of about 4.1 KB.

| one read | Push 3 (x86) | built for Move, run on an ARM Mac |
|---|---|---|
| `mod_map` | 15–18 µs, no allocation | about 20 µs, no allocation |
| `live_mod`, three controls moving | about 1 µs | about 1 µs |

`mod_set` allocates inside Surge when a route is added (its route lists grow), as Surge's existing
modulation slots do.

## Not specified

- A way for a module to say which of the keys it has without being asked (a line in `module.json`).
- Modulation a module receives from the chain host: that is the host's (`:effective`, `:base`,
  `:modulated`).
