(* animator.ml — functional, recursive animation tool for the 80x7 bi-colour
   LED panel.

   THE CORE IDEA
   An animation is a function of time and position, not an array of frames:

       type anim = float -> int -> int -> pixel

   Every effect is a function from anim to anim, so they compose. Recursion is
   the natural way to build trails, cellular automata, and anything iterative.
   Frames only exist at the very end, when [rasterise] samples the function at a
   fixed frame rate and [emit_header] writes frames.h.

   BUILD
       ocamlfind ocamlopt -package str -linkpkg animator.ml -o animator
     or, with no ocamlfind:
       ocamlopt str.cmxa animator.ml -o animator

   USAGE
       ./animator demo    frames.h            built-in demo reel
       ./animator pan     in.pan  frames.h    rasterise a hand-drawn .pan file
       ./animator show    in.pan              colour preview in the terminal
       ./animator life    frames.h  [seconds] Game of Life on the panel
       ./animator plasma  frames.h  [seconds]
       ./animator marquee frames.h  "TEXT"    scrolling text

   PANEL CONSTRAINTS
   4 colours (off/red/green/amber), 4 brightness levels (0..3). Anything that
   relies on smooth gradients will band badly at 4 levels, so effects here are
   designed for the quantisation rather than dithered down to it.

   OUTPUT FORMAT (matches firmware renderFrame())
   One byte per pixel, 80 x 7 = 560 bytes per frame, rows TOP-FIRST, columns
   LEFT-FIRST as a viewer sees the panel. bits 3:2 colour, bits 1:0 level.
   The firmware applies the geometry inversions. *)

let width = 80
let height = 7

(* ------------------------------------------------------------------------ *)
(* Pixels                                                                    *)
(* ------------------------------------------------------------------------ *)

type colour = Off | Red | Green | Amber

type pixel = { colour : colour; level : int }   (* level 0..3 *)

let off = { colour = Off; level = 0 }
let px c l = if l <= 0 || c = Off then off else { colour = c; level = min 3 l }

let opaque p = p.colour <> Off && p.level > 0

let colour_code = function Off -> 0 | Red -> 1 | Green -> 2 | Amber -> 3

let encode p =
  if not (opaque p) then 0 else (colour_code p.colour lsl 2) lor p.level

(* Mix two colours additively, the way the panel does: red + green = amber. *)
let mix a b =
  match a, b with
  | Off, c | c, Off -> c
  | Red, Green | Green, Red -> Amber
  | Amber, _ | _, Amber -> Amber
  | c, _ -> c

(* ------------------------------------------------------------------------ *)
(* Animations as functions                                                   *)
(* ------------------------------------------------------------------------ *)

type anim = float -> int -> int -> pixel
(* time in seconds, x 0..79 left-to-right, y 0..6 top-to-bottom *)

let blank : anim = fun _ _ _ -> off

let solid c l : anim = fun _ _ _ -> px c l

(* ---- spatial ---- *)

let shift dx dy (a : anim) : anim = fun t x y -> a t (x - dx) (y - dy)

let wrap_x (a : anim) : anim =
  fun t x y -> a t (((x mod width) + width) mod width) y

let clip (a : anim) : anim =
  fun t x y -> if x < 0 || x >= width || y < 0 || y >= height then off else a t x y

let flip_x (a : anim) : anim = fun t x y -> a t (width - 1 - x) y
let flip_y (a : anim) : anim = fun t x y -> a t x (height - 1 - y)

(* ---- temporal ---- *)

let delay d (a : anim) : anim = fun t x y -> if t < d then off else a (t -. d) x y

let loop period (a : anim) : anim =
  fun t x y -> a (mod_float t period) x y

let seq d (a : anim) (b : anim) : anim =
  fun t x y -> if t < d then a t x y else b (t -. d) x y

let speed k (a : anim) : anim = fun t x y -> a (t *. k) x y

(* ---- brightness / colour ---- *)

let dim n (a : anim) : anim =
  fun t x y -> let p = a t x y in px p.colour (p.level - n)

let brighten n (a : anim) : anim =
  fun t x y -> let p = a t x y in px p.colour (p.level + n)

let recolour c (a : anim) : anim =
  fun t x y -> let p = a t x y in if opaque p then px c p.level else off

(* ---- compositing ---- *)

let over (top : anim) (bot : anim) : anim =
  fun t x y -> let p = top t x y in if opaque p then p else bot t x y

let add (a : anim) (b : anim) : anim =
  fun t x y ->
    let p = a t x y and q = b t x y in
    px (mix p.colour q.colour) (max p.level q.level)

(* Crossfade over [d] seconds by quantised brightness. *)
let crossfade d (a : anim) (b : anim) : anim =
  fun t x y ->
    let k = if d <= 0. then 1. else min 1. (max 0. (t /. d)) in
    let p = a t x y and q = b t x y in
    let pl = int_of_float (float p.level *. (1. -. k) +. 0.5)
    and ql = int_of_float (float q.level *. k +. 0.5) in
    if ql >= pl then px q.colour ql else px p.colour pl

(* ---- recursion: trails ---- *)

(* A trail is defined recursively: the sprite, over a dimmer copy of the trail
   one step behind. [n] bounds the recursion. *)
let rec trail n dx (a : anim) : anim =
  if n <= 0 then a
  else over a (dim 1 (shift dx 0 (trail (n - 1) dx a)))

(* ------------------------------------------------------------------------ *)
(* Sprites and text                                                          *)
(* ------------------------------------------------------------------------ *)

(* Sprites are small pixel grids, drawn at the origin. *)
type sprite = pixel array array   (* [y][x] *)

let sprite_anim (s : sprite) : anim =
  fun _ x y ->
    if y < 0 || y >= Array.length s then off
    else if x < 0 || x >= Array.length s.(y) then off
    else s.(y).(x)

(* Two characters per pixel, as in .pan files: "r3" bright red, ".." off. *)
let parse_pixels (line : string) : pixel array =
  let n = String.length line / 2 in
  Array.init n (fun i ->
    let c = line.[2 * i] and l = line.[2 * i + 1] in
    let colour = match c with 'r' -> Red | 'g' -> Green | 'a' -> Amber | _ -> Off in
    let level = if l >= '1' && l <= '3' then Char.code l - Char.code '0' else 0 in
    px colour level)

let sprite_of_lines (lines : string list) : sprite =
  Array.of_list (List.map parse_pixels lines)

(* 5x7 font, one row per byte, bit 4 = leftmost column. *)
let font5x7 c =
  match c with
  | '0' -> [|0x0E;0x11;0x13;0x15;0x19;0x11;0x0E|]
  | '1' -> [|0x04;0x0C;0x04;0x04;0x04;0x04;0x0E|]
  | '2' -> [|0x0E;0x11;0x01;0x02;0x04;0x08;0x1F|]
  | '3' -> [|0x1F;0x02;0x04;0x02;0x01;0x11;0x0E|]
  | '4' -> [|0x02;0x06;0x0A;0x12;0x1F;0x02;0x02|]
  | '5' -> [|0x1F;0x10;0x1E;0x01;0x01;0x11;0x0E|]
  | '6' -> [|0x06;0x08;0x10;0x1E;0x11;0x11;0x0E|]
  | '7' -> [|0x1F;0x01;0x02;0x04;0x08;0x08;0x08|]
  | '8' -> [|0x0E;0x11;0x11;0x0E;0x11;0x11;0x0E|]
  | '9' -> [|0x0E;0x11;0x11;0x0F;0x01;0x02;0x0C|]
  | 'A' -> [|0x0E;0x11;0x11;0x1F;0x11;0x11;0x11|]
  | 'B' -> [|0x1E;0x11;0x11;0x1E;0x11;0x11;0x1E|]
  | 'C' -> [|0x0E;0x11;0x10;0x10;0x10;0x11;0x0E|]
  | 'D' -> [|0x1C;0x12;0x11;0x11;0x11;0x12;0x1C|]
  | 'E' -> [|0x1F;0x10;0x10;0x1E;0x10;0x10;0x1F|]
  | 'F' -> [|0x1F;0x10;0x10;0x1E;0x10;0x10;0x10|]
  | 'G' -> [|0x0E;0x11;0x10;0x17;0x11;0x11;0x0F|]
  | 'H' -> [|0x11;0x11;0x11;0x1F;0x11;0x11;0x11|]
  | 'I' -> [|0x0E;0x04;0x04;0x04;0x04;0x04;0x0E|]
  | 'J' -> [|0x07;0x02;0x02;0x02;0x02;0x12;0x0C|]
  | 'K' -> [|0x11;0x12;0x14;0x18;0x14;0x12;0x11|]
  | 'L' -> [|0x10;0x10;0x10;0x10;0x10;0x10;0x1F|]
  | 'M' -> [|0x11;0x1B;0x15;0x15;0x11;0x11;0x11|]
  | 'N' -> [|0x11;0x11;0x19;0x15;0x13;0x11;0x11|]
  | 'O' -> [|0x0E;0x11;0x11;0x11;0x11;0x11;0x0E|]
  | 'P' -> [|0x1E;0x11;0x11;0x1E;0x10;0x10;0x10|]
  | 'Q' -> [|0x0E;0x11;0x11;0x11;0x15;0x12;0x0D|]
  | 'R' -> [|0x1E;0x11;0x11;0x1E;0x14;0x12;0x11|]
  | 'S' -> [|0x0F;0x10;0x10;0x0E;0x01;0x01;0x1E|]
  | 'T' -> [|0x1F;0x04;0x04;0x04;0x04;0x04;0x04|]
  | 'U' -> [|0x11;0x11;0x11;0x11;0x11;0x11;0x0E|]
  | 'V' -> [|0x11;0x11;0x11;0x11;0x11;0x0A;0x04|]
  | 'W' -> [|0x11;0x11;0x11;0x15;0x15;0x15;0x0A|]
  | 'X' -> [|0x11;0x11;0x0A;0x04;0x0A;0x11;0x11|]
  | 'Y' -> [|0x11;0x11;0x11;0x0A;0x04;0x04;0x04|]
  | 'Z' -> [|0x1F;0x01;0x02;0x04;0x08;0x10;0x1F|]
  | ' ' -> [|0;0;0;0;0;0;0|]
  | '-' -> [|0;0;0;0x1F;0;0;0|]
  | '.' -> [|0;0;0;0;0;0x0C;0x0C|]
  | ':' -> [|0;0x0C;0x0C;0;0x0C;0x0C;0|]
  | '!' -> [|0x04;0x04;0x04;0x04;0x04;0;0x04|]
  | _   -> [|0x1F;0x1F;0x1F;0x1F;0x1F;0x1F;0x1F|]  (* unknown = block *)

(* Text as an anim at the origin, 6 pixels per character (5 + 1 gap). *)
let text c l (s : string) : anim =
  let s = String.uppercase_ascii s in
  fun _ x y ->
    if y < 0 || y >= 7 || x < 0 then off
    else
      let i = x / 6 and col = x mod 6 in
      if col = 5 || i >= String.length s then off
      else
        let row = (font5x7 s.[i]).(y) in
        if row land (0x10 lsr col) <> 0 then px c l else off

let text_width s = 6 * String.length s

(* Scrolling marquee: enters from the right, exits left, loops. *)
let marquee c l speed (s : string) : anim =
  let w = text_width s in
  let period = float (w + width) /. speed in
  loop period (fun t x y ->
    let dx = width - int_of_float (t *. speed) in
    text c l s t (x - dx) y)

(* ------------------------------------------------------------------------ *)
(* Recursive / generative sources                                            *)
(* ------------------------------------------------------------------------ *)

(* Cellular automaton: a grid stepped by a rule, sampled at [fps] steps/sec.
   Defined recursively over generation number, memoised so each generation is
   computed once. *)
let automaton ~(step : bool array array -> bool array array)
              ~(init : bool array array) ~(fps : float)
              ~(paint : int -> int -> bool -> pixel) : anim =
  let cache = Hashtbl.create 64 in
  let rec gen n =
    if n <= 0 then init
    else match Hashtbl.find_opt cache n with
      | Some g -> g
      | None -> let g = step (gen (n - 1)) in Hashtbl.add cache n g; g
  in
  fun t x y ->
    if x < 0 || x >= width || y < 0 || y >= height then off
    else
      let g = gen (int_of_float (t *. fps)) in
      paint x y g.(y).(x)

(* Conway's Life on a torus — 80x7 is small enough that most patterns die or
   settle fast, which makes a good "reset every few seconds" loop. *)
let life_step g =
  let h = Array.length g and w = Array.length g.(0) in
  Array.init h (fun y -> Array.init w (fun x ->
    let n = ref 0 in
    for dy = -1 to 1 do for dx = -1 to 1 do
      if dy <> 0 || dx <> 0 then begin
        let yy = ((y + dy) mod h + h) mod h and xx = ((x + dx) mod w + w) mod w in
        if g.(yy).(xx) then incr n
      end
    done done;
    if g.(y).(x) then !n = 2 || !n = 3 else !n = 3))

let random_grid density =
  Array.init height (fun _ -> Array.init width (fun _ -> Random.float 1. < density))

(* Plasma: sums of sines, quantised to 4 levels and coloured by band. Designed
   for the panel's quantisation rather than dithered — bands are the look. *)
let plasma : anim =
  fun t x y ->
    let fx = float x and fy = float y in
    let v = sin (fx *. 0.18 +. t *. 1.3)
          +. sin (fy *. 0.7 +. t *. 0.9)
          +. sin ((fx +. fy) *. 0.12 +. t *. 0.7)
          +. sin (sqrt (fx *. fx *. 0.02 +. fy *. fy *. 0.5) -. t *. 1.1) in
    let u = (v +. 4.) /. 8. in                (* 0..1 *)
    let band = int_of_float (u *. 6.) in       (* 0..5 *)
    match band with
    | 0 -> off
    | 1 -> px Red 1   | 2 -> px Red 3
    | 3 -> px Amber 2 | 4 -> px Green 2
    | _ -> px Green 3

(* ------------------------------------------------------------------------ *)
(* Rasterising and output                                                    *)
(* ------------------------------------------------------------------------ *)

type frame = pixel array array   (* [y][x] *)

let sample (a : anim) t : frame =
  Array.init height (fun y -> Array.init width (fun x -> a t x y))

(* SMOOTH MOTION RULE: for anything that moves in whole pixels, pick a speed
   that is an exact multiple of fps (e.g. 20 or 40 px/s at 20 fps). Fractional
   pixels per frame alternate between step sizes and look like stutter. The
   firmware's ANIM_FRAME_MS must equal 1000/fps or frames get dropped/repeated. *)
let rasterise (a : anim) ~fps ~seconds : frame list =
  let n = int_of_float (fps *. seconds) in
  List.init n (fun i -> sample a (float i /. fps))

let emit_header (frames : frame list) path =
  let oc = open_out path in
  let p fmt = Printf.fprintf oc fmt in
  p "// AUTO-GENERATED by tools/animator.ml - do not edit by hand.\n//\n";
  p "// One byte per pixel, %d columns x %d rows = %d bytes per frame.\n"
    width height (width * height);
  p "//     bits 3:2  colour  0 = off, 1 = red, 2 = green, 3 = amber\n";
  p "//     bits 1:0  level   0..3 brightness (0 is off regardless)\n//\n";
  p "// Rows TOP-FIRST, columns LEFT-FIRST as a viewer sees the panel.\n";
  p "// The firmware applies the geometry inversions.\n\n";
  p "#define ANIM_FRAME_COUNT %d\n\n" (List.length frames);
  p "static const uint8_t animFrames[ANIM_FRAME_COUNT][%d] = {\n" (width * height);
  List.iter (fun fr ->
    p "  {\n";
    Array.iter (fun row ->
      p "    ";
      Array.iteri (fun i px ->
        if i > 0 then p ",";
        p "0x%02X" (encode px)) row;
      p ",\n") fr;
    p "  },\n") frames;
  p "};\n";
  close_out oc;
  let total = List.length frames * width * height in
  Printf.printf "wrote %s: %d frame(s), %d bytes\n" path (List.length frames) total;
  if total > 1_500_000 then
    print_endline "WARNING: close to the RP2040-Zero 2MB flash limit."

(* ---- .pan files ---- *)

let read_pan path : frame list =
  let ic = open_in path in
  let frames = ref [] and cur = ref [] in
  let flush () =
    if !cur <> [] then begin
      let rows = Array.of_list (List.rev_map parse_pixels !cur) in
      let fr = Array.init height (fun y ->
        Array.init width (fun x ->
          if y < Array.length rows && x < Array.length rows.(y) then rows.(y).(x) else off)) in
      frames := fr :: !frames; cur := []
    end in
  (try while true do
    let line = input_line ic in
    if String.trim line = "---" then flush ()
    else if String.trim line <> "" then cur := line :: !cur
  done with End_of_file -> ());
  flush (); close_in ic;
  List.rev !frames

(* A .pan file becomes an anim: frame i shown for 1/fps seconds, looping. *)
let pan_anim (frames : frame list) ~fps : anim =
  let arr = Array.of_list frames in
  let n = Array.length arr in
  if n = 0 then blank
  else fun t x y ->
    let i = (int_of_float (t *. fps)) mod n in
    if x < 0 || x >= width || y < 0 || y >= height then off else arr.(i).(y).(x)

(* ---- terminal preview ---- *)

let ansi p =
  if not (opaque p) then "\027[90m\183\027[0m"
  else
    let code = match p.colour, p.level with
      | Red, 1 -> "31" | Red, 2 -> "31;1" | Red, _ -> "91;1"
      | Green, 1 -> "32" | Green, 2 -> "32;1" | Green, _ -> "92;1"
      | Amber, 1 -> "33" | Amber, 2 -> "33;1" | Amber, _ -> "93;1"
      | _ -> "0" in
    let block = match p.level with 1 -> "\226\150\145" | 2 -> "\226\150\146" | _ -> "\226\150\136" in
    "\027[" ^ code ^ "m" ^ block ^ "\027[0m"

let show (frames : frame list) =
  List.iteri (fun i fr ->
    Printf.printf "--- frame %d ---\n" (i + 1);
    Array.iter (fun row ->
      Array.iter (fun p -> print_string (ansi p)) row; print_newline ()) fr) frames

(* ------------------------------------------------------------------------ *)
(* Demo reel — shows the combinators composing                               *)
(* ------------------------------------------------------------------------ *)

let demo : anim =
  (* A bright bar with a recursive 3-deep trail, sweeping right and wrapping. *)
  let bar c = fun _ x y -> if x = 0 && y >= 0 && y < height then px c 3 else off in
  (* 40 px/s at 20 fps = exactly 2 px per frame. Fractional steps (e.g. 24 px/s =
     1.2 px/frame) alternate between 1- and 2-pixel jumps and read as stutter. *)
  let sweep c = wrap_x (fun t x y -> trail 3 (-1) (bar c) t (x - int_of_float (t *. 40.)) y) in
  (* Three colours in sequence, each for 3.3s, then a marquee, then plasma. *)
  let sweeps =
    seq 3.3 (sweep Red) (seq 3.3 (sweep Green) (sweep Amber)) in
  let words = marquee Amber 3 20. "  FLUX STAINED HEX  " in   (* 1 px/frame *)
  seq 10. sweeps (seq 8. words (speed 1. plasma))



(* ------------------------------------------------------------------------ *)
(* Plasma variants for the PIO driver (50 fps, all four levels)              *)
(* ------------------------------------------------------------------------ *)
(* The original [plasma] was tuned for 20 fps and paints one colour band per
   pixel. These run at 50 fps and use TWO independent fields - one for red, one
   for green - added with the panel's own additive mixing, so amber appears
   wherever the fields overlap rather than being a fixed band. Brightness comes
   from each field's own amplitude, quantised to the 4 levels. *)

(* A generic field: sum of drifting sines in 0..1. *)
let field ~fx ~fy ~fd ~s1 ~s2 ~s3 : float -> int -> int -> float =
  fun t x y ->
    let x = float x and y = float y in
    let v = sin (x *. fx +. t *. s1)
          +. sin (y *. fy +. t *. s2)
          +. sin ((x *. 0.5 +. y) *. fd -. t *. s3)
          +. sin (sqrt (x *. x *. 0.01 +. y *. y *. 0.3) +. t *. s1 *. 0.7) in
    (v +. 4.) /. 8.

let quant4 u = max 0 (min 3 (int_of_float (u *. 4.)))

(* Two-field plasma: red and green fields drift at different rates, so the
   colour composition itself evolves - amber blooms and dissolves. *)
let plasma_dual : anim =
  let fr = field ~fx:0.16 ~fy:0.6  ~fd:0.11 ~s1:1.1 ~s2:0.8 ~s3:0.6 in
  let fg = field ~fx:0.13 ~fy:0.75 ~fd:0.09 ~s1:0.7 ~s2:1.2 ~s3:0.9 in
  fun t x y ->
    let r = quant4 (fr t x y -. 0.15)   (* bias so there is some dark *)
    and g = quant4 (fg (t +. 3.) x y -. 0.15) in
    px (mix (if r > 0 then Red else Off) (if g > 0 then Green else Off)) (max r g)

(* Interference plasma: one field, but colour chosen by the SLOPE of the field
   rather than its value, which gives moving contour lines instead of blobs. *)
let plasma_contour : anim =
  let f = field ~fx:0.2 ~fy:0.5 ~fd:0.14 ~s1:0.9 ~s2:1.0 ~s3:0.7 in
  fun t x y ->
    let v = f t x y in
    let dv = f t (x + 1) y -. v in            (* horizontal slope *)
    let level = quant4 v in
    if level = 0 then off
    else if dv > 0.03 then px Red level
    else if dv < -0.03 then px Green level
    else px Amber level

(* Pulse plasma: the whole field breathes in brightness on a slow envelope
   while its shape drifts, so the panel dims to nothing and blooms back. *)
let plasma_pulse : anim =
  let f = field ~fx:0.14 ~fy:0.65 ~fd:0.1 ~s1:1.3 ~s2:0.9 ~s3:0.5 in
  fun t x y ->
    let env = (sin (t *. 0.8) +. 1.) /. 2. in     (* 0..1, ~8s period *)
    let v = f t x y *. env in
    let level = quant4 v in
    let c = match int_of_float (t /. 4.) mod 3 with 0 -> Amber | 1 -> Green | _ -> Red in
    px c level

let plasma_reel : anim =
  seq 10. plasma_dual (seq 10. plasma_contour plasma_pulse)

(* ------------------------------------------------------------------------ *)
(* Showcase — designed to be IMPOSSIBLE on the bit-banged driver             *)
(* ------------------------------------------------------------------------ *)
(* Rasterised at 50 fps (set ANIM_FRAME_MS 20 in the firmware). At the old
   ~46Hz repaint with 4 BCM levels, 50 fps playback would drop frames and every
   fast edge would strobe. With PIO the repaint is several hundred Hz, so this
   should be fluid.

   Sections:
     1. Brightness ladder  - 12 static bands: 3 colours x 4 levels. Proves the
                             four levels are distinct and stable. If you see only
                             two distinct brightnesses, BCM_LEVELS is 2.
     2. Fast bouncer       - single bright pixel at 100 px/s with a 6-deep trail
                             fading through all four levels. Strobes at low
                             refresh; draws a smooth comet at high refresh.
     3. Fast marquee       - text at 50 px/s (1 px/frame at 50 fps). The original
                             demo was 20 px/s.
     4. Colour sweep       - a wipe that crosses the panel 4 times a second. *)

let ladder : anim =
  fun _ x _ ->
    (* 80 columns / 12 bands = 6.67 px each. Band index 0..11. *)
    let band = min 11 (x * 12 / width) in
    let colour = match band / 4 with 0 -> Red | 1 -> Green | _ -> Amber in
    let level = band mod 4 in
    px colour level

let bouncer : anim =
  (* Triangle wave over 0..79 at 100 px/s: one full bounce every 1.6s. *)
  let pos t =
    let p = mod_float (t *. 100.) (2. *. float (width - 1)) in
    if p < float (width - 1) then int_of_float p
    else int_of_float (2. *. float (width - 1) -. p) in
  let dir t =   (* +1 moving right, -1 moving left *)
    let p = mod_float (t *. 100.) (2. *. float (width - 1)) in
    if p < float (width - 1) then 1 else -1 in
  let head c = fun _ x y -> if x = 0 && y = 3 then px c 3 else off in
  fun t x y ->
    let c = match int_of_float (t /. 1.6) mod 3 with 0 -> Amber | 1 -> Red | _ -> Green in
    (* trail extends BEHIND the direction of travel *)
    let tr = trail 6 (- (dir t)) (head c) in
    tr t (x - pos t) y

let fast_marquee = marquee Green 3 50. "  PIO  DRIVER  "

let sweep : anim =
  fun t x _ ->
    (* Wipe crossing the whole panel 4 times a second, colour changing each pass. *)
    let pass = int_of_float (t *. 4.) in
    let edge = int_of_float (mod_float (t *. 4.) 1. *. float width) in
    let c = match pass mod 3 with 0 -> Red | 1 -> Amber | _ -> Green in
    if x <= edge then px c (if edge - x < 6 then 3 else if edge - x < 14 then 2 else 1)
    else off

let showcase : anim =
  seq 3. ladder (seq 5. bouncer (seq 5. fast_marquee sweep))

(* ------------------------------------------------------------------------ *)
(* Entry point                                                               *)
(* ------------------------------------------------------------------------ *)

let usage () =
  print_endline "usage:\n\
  \  animator demo     frames.h\n\
  \  animator showcase frames.h        (50 fps - set ANIM_FRAME_MS 20)\n\
  \  animator pan     in.pan  frames.h\n\
  \  animator show    in.pan\n\
  \  animator life    frames.h [seconds]\n\
  \  animator plasma  frames.h [seconds]       (20 fps)\n\
  \  animator plasma2 frames.h [seconds]       (50 fps - set ANIM_FRAME_MS 20)\n\
  \  animator marquee frames.h \"TEXT\""

let () =
  Random.self_init ();
  let args = Array.to_list Sys.argv |> List.tl in
  let secs_or d = function s :: _ -> (try float_of_string s with _ -> d) | [] -> d in
  match args with
  | ["demo"; out] ->
      emit_header (rasterise demo ~fps:20. ~seconds:24.) out
  | ["showcase"; out] ->
      (* 50 fps: set ANIM_FRAME_MS 20 in the firmware to match. *)
      emit_header (rasterise showcase ~fps:50. ~seconds:16.) out
  | ["pan"; inp; out] ->
      emit_header (read_pan inp) out
  | ["show"; inp] ->
      show (read_pan inp)
  | "life" :: out :: rest ->
      let a = automaton ~step:life_step ~init:(random_grid 0.35) ~fps:8.
                ~paint:(fun _ _ alive -> if alive then px Green 3 else off) in
      emit_header (rasterise a ~fps:8. ~seconds:(secs_or 20. rest)) out
  | "plasma" :: out :: rest ->
      emit_header (rasterise plasma ~fps:20. ~seconds:(secs_or 10. rest)) out
  | "plasma2" :: out :: rest ->
      (* 50 fps: dual-field, contour and pulse plasmas. ANIM_FRAME_MS 20. *)
      emit_header (rasterise plasma_reel ~fps:50. ~seconds:(secs_or 30. rest)) out
  | ["marquee"; out; s] ->
      (* 20 px/s at 20 fps = 1 px/frame, so scrolling is perfectly even. *)
      let a = marquee Amber 3 20. s in
      emit_header (rasterise a ~fps:20.
                     ~seconds:(float (text_width s + width) /. 20.)) out
  | _ -> usage ()
