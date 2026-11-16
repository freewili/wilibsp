# Emberward

Guard the core. Sparks fly at a glowing core from eight directions; you turn
a quarter-circle ward around it. Block the red embers. Let the golden glims
through: they feed the core and charge your flare. Waves get faster and
denser until the core goes dark.

Play it on a TV: plug the FreeWili 2's TV connector into one of the TV's inputs.
The LCD becomes the companion screen with your score, lives, flare meter,
the high-score table and the button labels. No TV? Press GREY for LCD view.

**Emberward has not yet been run on a board.** It builds, and its game
rules, key repeat, score table, sound and TV mixer pass host tests.

## Controls

| | Front panel | Nav pad | TV remote |
|---|---|---|---|
| Turn the ward left / right | YELLOW / BLUE | LEFT / RIGHT | LEFT / RIGHT |
| Flip the ward half a turn | | UP / DOWN | UP / DOWN |
| Fire the flare, start, confirm | GREEN | OK | SELECT or PLAY |
| Pause (pause again: quit) | RED | CANCEL | PAUSE, STOP or EXIT |
| TV view / LCD view | GREY | | |

The remote's yellow, green, blue and red keys do what the front-panel button
of the same colour does on that screen. Hold a turn key to keep turning. The button labels on the LCD can be
touched too; in LCD view touch the left third to turn left, the right third
to turn right and the middle for OK.

## Sparks

- **Ember** (red): block it with the ward. One that reaches the core costs a
  life, then you have a moment of grace.
- **Glim** (gold): let it in. Blocking it shatters it and breaks your combo.
- **Dart** (cyan, from wave 3): a fast ember.
- **Heart** (green, from wave 4, rare): let it in for an extra life (five at
  most).

From wave 2 sparks also come in adjacent pairs: one ward position always
answers both.

## Scoring

Every correct call (ember blocked, glim or heart let in) grows your combo; the
multiplier is x1, rising by one every eight calls up to x5. Ember 10, dart 20,
glim 25, heart 50, all times the multiplier, and 100 x wave for clearing a
wave. Five glims fill the flare: press OK to burn every spark on the screen
in your favour. A top-five score asks for your initials (UP/DOWN pick a
letter, LEFT/RIGHT move, OK on the OK box) and is saved on the SD card in
`/appdata/emberward/`.

## TV remote

Emberward joins the TV's CEC bus as "Emberward", turns the TV on and
switches it to its input. Your TV's CEC setting (each brand names it
differently) must be on for the remote's keys to reach the game. Started
from the FreeWili menu, the game knows its input from the TV itself. Started
another way, it uses the input saved on the SD card by any FreeWili app; when
the TV is then switched to the game's input with the TV's remote, the game
takes that input and saves it. To set it by hand, choose the input with BLUE
(TV INPUT) on the title screen. The game pauses when the TV goes to standby or
to another input.

## Sound

With a TV connected and TV sound on, the sound comes out of the TV and the
board's speaker stays silent; several effects play at once. Without a TV,
or with TV sound off, the speaker plays them. The game switches the TV
signal to one that carries sound only once the TV connector reports a TV:
the picture blinks once as it does. A plain DVI monitor (no speakers) may
show nothing with that signal: turn TV sound off (RED on the title
screen). If the board never reports the TV (older coprocessor firmware),
the speaker plays.

**TV sound is unverified on the board**: nobody has heard it on a TV yet. Over RTT the game prints the TV
sound statistics every ten seconds (underruns, late, lost, repeats).

## Title screen

GREEN starts, YELLOW turns sound on or off, BLUE sets the TV input number
(saved for every FreeWili app), RED turns TV sound on or off, GREY switches
the view. Sound and TV sound are saved with the scores. Hold HOME
for five seconds to leave the game; hold PAGE for five seconds for the About
page.
