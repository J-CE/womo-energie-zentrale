// ======================================================================
//  Womo Energy Core v5.0 – Gehäuse v3 (parametrisch, 3D-Druck)
//  Neu nach Vorgabe:
//   - Platine 160 x 100 mm, mittig in der Box (5mm Rand umlaufend)
//   - Box-Grundfläche (außen):  170 x 110 mm
//   - Platinenhalter-Lochabstand: 151 x 91 mm (= 4.5mm Inset auf der Platine)
//   - Platinenhalter-Höhe: 8 mm
//   - Gesamtinnenhöhe (Boden -> Deckel-Unterseite): 70 mm
//   - Deckel-Befestigung: Rastnasen (Schnapp-Verbindung), NICHT verschraubt,
//     daher unabhängig vom schmalen 5mm-Rand realisierbar
//   - Kabeldurchlässe: Unterkante ab 11mm über dem Boden
//
//  Achsen (wie in der Aufbau-Skizze):
//    X = kurze Kante (110mm außen / 100mm Platine), Y = lange Kante (170/160mm)
//    y=0   : OBEN  (Stromkabel-Eingang)        y=Y_out : UNTEN (Landstrom/Klemmen)
//    x=0   : LINKS (MPPT / Gel-Lader / RS485)  x=X_out : RECHTS (Antenne/USB-C/D+)
//
//  WICHTIG: Kabeldurchlass-Positionen (x/Breite) sind weiterhin Schätzungen
//  anhand der Fotos/Skizze – bitte vor dem Druck am Board nachmessen!
//
//  STL-Export:  openscad -o base.stl this.scad -D part="base"
//               openscad -o lid.stl  this.scad -D part="lid"
// ======================================================================

/* [Platine] */
board_X = 100;   // kurze Kante
board_Y = 160;   // lange Kante
board_T = 1.6;

/* [Box – fest vorgegeben] */
X_out = 110;     // Außenmaß Box, kurze Achse
Y_out = 170;     // Außenmaß Box, lange Achse
margin = (X_out-board_X)/2; // = 5mm, ergibt sich aus den Vorgaben (zentriert)

/* [Platinenhalter] */
standoff_spacing_X = 91;   // 151x91 Lochabstand
standoff_spacing_Y = 151;
standoff_h   = 8;          // Höhe der Platinenhalter
standoff_d   = 7;
screw_pilot_d= 2.6;        // M3 selbstschneidend

/* [Gesamtmaße] */
wall_t   = 2.4;
corner_r = 6;              // jetzt frei wählbar (keine Schraubdome mehr in der Ecke)
floor_t  = 2.6;
lid_top_t= 2.6;
lid_skirt= 8;
fit_gap  = 0.25;
internal_H = 60;           // Gesamtinnenhöhe Boden -> Deckel-Unterseite (FEST)
base_wall_h = floor_t + internal_H; // Außenhöhe der Wanne

/* [Kabeldurchlässe] – Unterkante ab 11mm über Boden (= floor_t+11 absolut)
   X/Y weiterhin Schätzungen anhand Skizze, Board-lokal (x:0..100, y:0..160) */
cut_z0 = floor_t + 12;

top_slot_x = 2;   top_slot_w = 43;  top_slot_h = 16;             // Eingang/Stromkabel (Wand OBEN)

left_mppt_y0 = 10;  left_mppt_y1 = 28;  left_mppt_d = 12;        // MPPT VE.Direct (Wand LINKS)
left_main_y0 = 72;  left_main_y1 = 124; left_main_h = 30;        // Gel-Lader + RS485 + VOUT li.

right_ant_y0 = 10;  right_ant_y1 = 28;  right_ant_d = 11;        // Antenne/Sensorkabel (Wand RECHTS)
right_usb_y0 = 40;  right_usb_y1 = 58;  right_usb_d = 11;        // USB-C Programmierkabel
right_main_y0= 72;  right_main_y1= 124; right_main_h = 30;       // D+ + VOUT re.

bottom_slot_x = 15; bottom_slot_w = 70; bottom_slot_h = 10;      // Landstrom + Klemmen (Wand UNTEN)

/* [Lüftung im Deckel über den MOSFET-Modulen] */
vent_count = 5;
vent_w     = 2.2;
vent_pitch = 3.6;
vent_len   = 26;

// LED-Sichtfenster im DECKEL (senkrechte Bohrung durch die Deckplatte,
// damit die onboard-RGB-LED von außen sichtbar ist).
// x/y = Position der LED auf der Platine, board-lokal (0..100 / 0..160) –
// GESCHÄTZT (ca. Mitte ESP32-Board), bitte am Board nachmessen!
led_x = 54;    // board-lokal, kurze Achse
led_y = 70;    // board-lokal, lange Achse
led_d = 11;

/* [Rastnasen / Schnapp-Verbindung Deckel<->Wanne] */
snap_r       = 1.7;   // Kugelradius Vertiefung (Wanne)
snap_r_lid   = 1.45;  // Kugelradius Nase (Deckel, etwas kleiner)
snap_below_top = 4;   // Abstand der Schnapp-Ebene von der Wannenoberkante
// Punkte auf dem AUSSENUMFANG der Wanne [x,y] – Lücken zu allen Kabeldurchlässen geprüft
snap_points = [
    [X_out*0.45, 0], [X_out*0.85, 0],          // Wand OBEN
    [X_out*0.15, Y_out], [X_out*0.75, Y_out],  // Wand UNTEN
    [0, Y_out*0.85],                            // Wand LINKS
    [X_out, Y_out*0.88],                        // Wand RECHTS
];

/* [Welcher Teil soll erzeugt werden?] */
part = "exploded"; // "base" | "lid" | "exploded" | "assembled"

// ======================================================================
module rounded_rect(l, w, r) {
    hull() {
        for (x = [r, l-r]) for (y = [r, w-r])
            translate([x, y]) circle(r=r, $fn=48);
    }
}

module shell(l, w, h, r, t) {
    difference() {
        linear_extrude(h) rounded_rect(l, w, r);
        translate([t, t, t]) linear_extrude(h) rounded_rect(l-2*t, w-2*t, max(r-t,0.5));
    }
}

// ---- Platinenhalter (4x, zentriert auf 91x151 Lochabstand) ----
module board_standoffs(h) {
    cx0 = X_out/2 - standoff_spacing_X/2;
    cx1 = X_out/2 + standoff_spacing_X/2;
    cy0 = Y_out/2 - standoff_spacing_Y/2;
    cy1 = Y_out/2 + standoff_spacing_Y/2;
    positions = [[cx0,cy0],[cx1,cy0],[cx0,cy1],[cx1,cy1]];
    for (p = positions) {
        translate([p[0], p[1], floor_t])
            difference() {
                cylinder(d=standoff_d, h=h, $fn=32);
                translate([0,0,-0.1]) cylinder(d=screw_pilot_d, h=h+0.2, $fn=24);
            }
    }
}

// ---- Kabel-/Klemmenausschnitte (Unterkante immer ab cut_z0) ----
module cutouts() {
    ox = margin;

    // Wand OBEN (y=0)
    //translate([ox+top_slot_x, -1, cut_z0])
    //    cube([top_slot_w, wall_t+2, top_slot_h]);

    // Wand LINKS (x=0)
    //translate([-1, ox+left_mppt_y0, cut_z0+left_mppt_d/2])
    //    rotate([0,90,0]) cylinder(d=left_mppt_d, h=wall_t+2, $fn=32);
    //translate([-1, ox+left_main_y0, cut_z0])
    //    cube([wall_t+2, left_main_y1-left_main_y0, left_main_h]);

    // Wand RECHTS (x=X_out)
    translate([X_out-wall_t-1, ox+right_ant_y0, cut_z0+right_ant_d/2])
        rotate([0,90,0]) cylinder(d=right_ant_d, h=wall_t+2, $fn=32);
    translate([X_out-wall_t-1, ox+right_usb_y0, cut_z0+right_usb_d/2])
        rotate([0,90,0]) cylinder(d=right_usb_d, h=wall_t+2, $fn=32);
    //translate([X_out-wall_t-1, ox+right_main_y0, cut_z0])
    //    cube([wall_t+2, right_main_y1-right_main_y0, right_main_h]);

    // Wand UNTEN (y=Y_out)
    translate([ox+bottom_slot_x, Y_out-wall_t-1, cut_z0])
        cube([bottom_slot_w, wall_t+2, bottom_slot_h]);
}

// ---- Rastnasen: Vertiefungen in der Wanne ----
module snap_dimples() {
    sz = base_wall_h - snap_below_top;
    for (p = snap_points)
        translate([p[0], p[1], sz]) sphere(r=snap_r, $fn=20);
}

// ---- Rastnasen: Nasen am Deckelrand (lid-lokale Koordinaten) ----
module snap_bumps() {
    sz_local = -snap_below_top; // relativ zur Deckel-Oberseite (lid z=0 = Wannenoberkante)
    for (p = snap_points)
        translate([p[0], p[1], sz_local]) sphere(r=snap_r_lid, $fn=20);
}

// ---- Basis-Wanne ----
module base_tray() {
    difference() {
        union() {
            shell(X_out, Y_out, base_wall_h, corner_r, wall_t);
            board_standoffs(standoff_h);
        }
        cutouts();
        snap_dimples();
    }
}

// ---- Deckel ----
module lid() {
    union() {
        difference() {
            union() {
                linear_extrude(lid_top_t) rounded_rect(X_out, Y_out, corner_r);
                translate([0,0,-lid_skirt])
                    difference() {
                        linear_extrude(lid_skirt) rounded_rect(X_out, Y_out, corner_r);
                        translate([wall_t+fit_gap, wall_t+fit_gap, -0.1])
                            linear_extrude(lid_skirt+0.2) rounded_rect(X_out-2*(wall_t+fit_gap), Y_out-2*(wall_t+fit_gap), max(corner_r-wall_t,0.5));
                    }
            }
            // Lüftungsschlitze über den MOSFET-Modulen (links: Gel-Lader, rechts: D+)
            for (i = [0:vent_count-1])
                translate([margin+3+i*vent_pitch, margin+100, -1]) cube([vent_w, vent_len, lid_top_t+2]);
            for (i = [0:vent_count-1])
                translate([X_out-margin-3-vent_w-i*vent_pitch, margin+100, -1]) cube([vent_w, vent_len, lid_top_t+2]);

            // LED-Sichtfenster: SENKRECHTE Bohrung durch die Deckplatte (Z-Achse),
            // an der Position der RGB-LED auf der Platine (margin = Versatz Board -> Box)
            translate([margin+led_x, margin+led_y, -1])
                cylinder(d=led_d, h=lid_top_t+2, $fn=32);
        }
        snap_bumps();
    }
}

// ======================================================================
// Ausgabe 
// ======================================================================
if (part == "base") {
    base_tray();
} else if (part == "lid") {
    translate([0,0,base_wall_h]) lid();
} else if (part == "assembled") {
    color("SlateGray") base_tray();
    color("LightSteelBlue", 0.95) translate([0,0,base_wall_h]) lid();
} else {
    color("SlateGray") base_tray();
    color("LightSteelBlue", 0.95) translate([0,0,base_wall_h + 45]) lid();
}
