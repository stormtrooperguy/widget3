# Claude rules for simon-says

## README maintenance

`README.md` must be kept in sync with the code. After any change to:

- `src/config.h` — update the configuration tables, GPIO summary, or wiring section if pins, defaults, or defines changed
- `src/main.cpp` — update the LED behaviour table, REST endpoints, or functionality description if logic changed
- `platformio.ini` — update the dependencies list if libraries were added or removed

Update the README in the same response as the code change, not as a separate follow-up.
