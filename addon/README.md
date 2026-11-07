# Race Broadcast (v0.5.1 beta)

Watch AI races like a TV broadcast. One addon combining:

- **AI drive**: press **F5** and the game's AI drives your car; press it again to take over, with a 3-second countdown. You can also set the AI to take the car from the start of the race.
- **Keep the race running after a crash**: when your car is wrecked the race goes on, the wreck goes to the garage, and you watch the rest.
- **Auto director**: the camera follows the action by itself, in this order:
  - incidents: spins, cars stopped on track, retirements;
  - overtakes;
  - cars side by side;
  - close battles, weighted towards the podium places;
  - otherwise the leader.

  The start, safety car restarts and red flag restarts stay on the leader for the whole lap unless there is a crash. It holds each shot for at least 9 seconds (adjustable). A story on screen is seen through: a fight stays until it is settled, a pass until the cars have separated, a pit stop until the car leaves the pit lane. When two things happen at once, the one nearer the front wins; only contact, a spin or a podium fight cuts a live fight short. **F3** turns the director off so you can use the game's own camera keys. **F4** turns the broadcast on or off by hand.
- **Broadcast graphics**:
  - a lower-third caption saying why the camera is on that car, for example "BATTLE FOR P4 · RUS 0.4s BEHIND HAM";
  - the game's own timing tower stays up while the camera follows other cars (an optional extra tower is in the settings);
  - LIVE and AI DRIVING tags.
- **With Project Aeternum** (the Project Aeternum addon, by Simracing Centre): while Aeternum runs, it draws the graphics and the director decides what is on screen:
  - the leaderboard stays as your Aeternum profile has it, and the track map is kept off;
  - the director brings in Battle For in a fight, Catching when a car is reeling in the one ahead, a driver card for a new car on camera, Pit Lane and then Tyre Life around a stop, Car Status for damage, Weather when it changes, Live Speed on your own car;
  - those graphics stack up from the bottom right corner (the leaderboard stays where your Aeternum layout puts it);
  - telemetry comes through Aeternum on port 20778 by itself, and our own captions, tower and tags stand down.

  Settings: "Graphics: use Project Aeternum", "Graphics: director runs Aeternum" and "Graphics: director places Aeternum's graphics".
- **Replays** (experimental, off by default, auto director only): contact, the start, restarts and overtakes that matter (the lead, the podium, P10, the last laps) are offered on screen; press **F9** to watch. The race pauses, the game's Instant Replay opens and the camera switches between the cars involved, then the race goes on. Or set Replays to Automatic. Replays work by opening the pause menu for you, so keep the game window in front, use the default keyboard keys, and don't touch the controls while one opens. They are switched off by themselves on a game version they were not made for.
- **Commentary** (beta, on by default at Normal; set it to Off, Quiet or Chatty in the settings): a four-person team. By default their voices are **recorded** (about 2,500 lines and every driver's name for each commentator, made with Google Gemini text to speech, in `commentary\voices.zip`): the driver's name plays just before a line about him, and the numbers are left to the on-screen graphics. Set "Commentary voices" to Piper for free offline voices that read every detail live (places, gaps, tyres):
  - **Ben Archer** (lead) calls the action and runs the broadcast;
  - **Graham Lowe** (expert) gives the driver's view, cuts in on racecraft and tyres, and talks you through each replay view by view;
  - **Tom Kirby** reports pit stops of the front runners (stop time, tyres, where they rejoin) and **Erin Doyle** covers the tyre picture on the formation lap, strategy calls, safety-car stops and explains penalties.

  With Project Aeternum they also talk about the graphics on screen: the gap on Battle For (and the tyres), a fight that goes on and on, how fast a car is catching, who the driver on the card is and how many places he has gained or lost, tyre age and wear on Tyre Life, the damage on Car Status, and a weather change.

  They are silent until the lights go out (all four preview the race on the formation lap), the call comes as the lights go out, and lines about a car wait until the camera shows it. Something big off camera is called as "elsewhere". Each level (Quiet, Normal, Chatty) has its own airtime budget, and no line is repeated within a race. It starts with the game and stops when it closes.

  What they talk about: the action on camera (passes, battles, contact with damage, spins, pit stops followed into the box and out, with the "will he come out in front?" moment); the safety car in three phases (coming out, the period itself, coming in and the restart); places changing in the pit stops called as the pit cycle, never as overtakes; the net race lead once the stops are taken into account; and, when it is quieter, the car on screen: tyres, pace, weather (much more when it is changeable), damage, teammate battles and team orders, the driver's career, the circuit's history, strategy, the race so far, how it might end, and short conversations between the four of them. Clean touches off camera are not called unless there is damage.
  With Piper, the first race sets up the voices: they are copied from your voice audition folder, or downloaded once (about 250 MB) into `addon-storage\apoc.race-broadcast\tts`. `commentary\Test voices.bat` plays one line from each voice without the game. Volume, speed and voices are in `commentary\commentary_settings.ini`. What was said is written to `addon-storage\apoc.race-broadcast\commentary.log`.

  These are original characters with invented names. They are not based on, and don't imitate, any real broadcaster.

**Turn on the game's UDP telemetry**: Settings > Telemetry > UDP Telemetry On, UDP IP 127.0.0.1, port 20777, format 2026 (with the 2026 Season Pack; 2025 also works). Project Aeternum needs 2026 for the new teams' colours and logos. The director reads positions, gaps and the game's own collision, overtake, retirement, fastest-lap and safety car events from it. AI drive works without it.

**Turn off AI Drive and Spectator/Broadcast Mode while this addon is on.** All three control your car and the spectator camera.

## Credits
- The "keep the race running" method (dropping the game's terminal-damage message and parking the wreck in the garage) comes from **Spectator/Broadcast Mode by Kizi & RK** (MIT; licence included). Their idea came from Terminal Damage by nyannoying.
- The AI drive part is written from scratch on Swofty's official `set_control` call, the same call AI Drive by Kizi uses.
- Thanks to Swofty for the launcher and the spectator, telemetry and overlay libraries.
- Voices: [Piper](https://github.com/rhasspy/piper) by Rhasspy (MIT) with the en_GB voice models aru, vctk and jenny_dioco from rhasspy/piper-voices (each under its own dataset licence, listed on its model card).

## Red flags
Under a red flag the addon steps back: the broadcast camera, the director and replays pause while the game runs its own red flag screens, and come back when the race restarts. If you take your car back (F5) during a red flag, it is handed over as soon as the game allows.

## Known limits (beta)
- Races only (other sessions are left alone). Tested on the Steam version in offline races through the Swofty Launcher. "Side by side" and "in trouble" are detected from positions and speeds, so a few false alarms are possible.
- The replay camera is always onboard for now.
- During a replay the expert talks over each view as it comes up, not frame by frame.
