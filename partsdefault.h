// partsdefault.h - the parts the game is built with: the text of a parts.txt (see partsdata.h)
#pragma once

static const char *const DEFAULT_PARTS_TEXT = R"PARTS(# Freehauler: hulls and ship parts.
#
# The numbers in a part's section are for size 1, standard grade. Each size up
# doubles what scales with size (mass, room, price, power, capacity...); each
# grade up does more and weighs less, as [rules] says.
#
# A ship's parts are saved by the order their models come in this file: add
# new models AFTER the ones already there, and don't reorder them.
# Online, your file must match the server's.

[rules]
grade_names = Standard, Improved, Advanced, Elite
grade_better = 0.15          # what each grade adds to what a part does
grade_leaner = 0.10          # what each grade takes off its mass (and off what a drive's field wastes)
grade_cost = 1, 2.5, 6, 15   # the price of each grade, against standard
stretch_room = 0.12          # a hull stretch: more room...
stretch_mass = 0.06          # ...more mass...
stretch_cost = 0.25          # ...and the first one's price, as a share of the hull's
stretch_cost_growth = 1.7    # each further stretch costs this many times the last
mount_mass = 0.03            # an extra hardpoint, likewise
mount_cost = 0.20
mount_cost_growth = 1.8

# --- Fuels: terajoules in a tonne -------------------------------------------
# What they really hold: fission fuel burned right up (8e10 MJ/t), and fusion fuel
# burned to helium (about 3.5e11 MJ/t: deuterium and helium-3 are much alike).
[fuel Reactor Fuel]
energy = 80000

[fuel Deuterium]
energy = 345000

[fuel Helium-3]
energy = 350000

# --- Hulls: room inside (m3), hardpoints outside, bare mass (t), price ------
[hull courier]
name = Courier
blurb = Small and quick: room for a good drive and not much else. Hard to see.
volume = 150
hardpoints = 6
mass = 10
cost = 8000

[hull hauler]
name = Hauler
blurb = The ship everyone starts in: a bit of everything.
volume = 400
hardpoints = 9
mass = 20
cost = 20000

[hull prospector]
name = Prospector
blurb = Hardpoints to spare for lasers, scoops and skimmers.
volume = 450
hardpoints = 12
mass = 26
cost = 32000

[hull cutter]
name = Cutter
blurb = A strong frame with mounts all round, built to chase and to fight.
volume = 600
hardpoints = 13
mass = 40
cost = 60000

[hull freighter]
name = Freighter
blurb = Four and a half times a hauler's room. Wants big engines.
volume = 1800
hardpoints = 11
mass = 90
cost = 110000

[hull bulk]
name = Bulk carrier
blurb = A flying warehouse, and it shows up like one.
volume = 6500
hardpoints = 13
mass = 320
cost = 400000

# --- Reactors ----------------------------------------------------------------
# A reactor makes heat at its burn rate times the energy in its fuel, and turns a
# share of that heat into power: its efficiency, which depends on how hard it is
# being run. The rest is waste heat, and it is what an infrared array sees.
#
#   burn      tonnes of fuel a second, flat out
#   floor     the share of that it can't be throttled below while it's lit: what
#             it makes then and nothing wants is dumped as heat
#   restart   seconds to light it again from cold
#   curve     efficiency against load (burn rate as a share of flat out), as
#             load:efficiency points joined by straight lines; the last at load 1.
#             Power must rise with load all the way.
#   efficiency_grade   what each grade adds to the whole curve
#
# The first three are the everyday reactor for each fuel (a ship's parts are
# saved by their order here: keep them first, in this order).
[reactor fission]
name = Fission reactor
mass = 6
volume = 15
cost = 3000
material = Reactor Cores
fuel = Reactor Fuel
burn = 0.16
floor = 0.03
restart = 15
curve = 0.05:0.1, 0.2:0.18, 0.5:0.24, 0.8:0.26, 1:0.25
efficiency_grade = 0.02

[reactor deuterium]
name = Deuterium fusion reactor
mass = 6
volume = 15
cost = 7500
material = Reactor Cores
fuel = Deuterium
burn = 0.0326
floor = 0.12
restart = 40
curve = 0.12:0.18, 0.3:0.28, 0.6:0.35, 0.85:0.38, 1:0.37
efficiency_grade = 0.02

[reactor helium3]
name = Helium-3 fusor
mass = 3.6
volume = 15
cost = 18000
material = Reactor Cores
fuel = Helium-3
burn = 0.0225
floor = 0.15
restart = 60
curve = 0.15:0.44, 0.3:0.55, 0.6:0.63, 0.85:0.66, 1:0.65
efficiency_grade = 0.02

# Little fuel goes through it, and it makes the most of it: but only near flat out.
[reactor fission_baseload]
name = Baseload fission reactor
mass = 7.8
volume = 21
cost = 4800
material = Reactor Cores
fuel = Reactor Fuel
burn = 0.0868
floor = 0.09
restart = 30
curve = 0.09:0.11, 0.4:0.19, 0.7:0.28, 0.9:0.29, 1:0.285
efficiency_grade = 0.02

# Burns hard for its size; wasteful throttled back.
[reactor fission_high]
name = High-output fission reactor
mass = 6.9
volume = 15
cost = 5400
material = Reactor Cores
fuel = Reactor Fuel
burn = 0.2896
floor = 0.075
restart = 10.5
curve = 0.075:0.05, 0.3:0.11, 0.6:0.18, 0.85:0.22, 1:0.23
efficiency_grade = 0.02

# Much the same efficiency at any load, and it idles on next to nothing.
[reactor fission_flat]
name = Load-following fission reactor
mass = 6.6
volume = 15
cost = 3900
material = Reactor Cores
fuel = Reactor Fuel
burn = 0.16
floor = 0.015
restart = 9
curve = 0.015:0.2, 0.1:0.225, 1:0.225
efficiency_grade = 0.02

# Half the room and little more than half the mass, and it wastes more of its fuel.
[reactor fission_compact]
name = Compact fission reactor
mass = 3.3
volume = 7.5
cost = 4200
material = Reactor Cores
fuel = Reactor Fuel
burn = 0.16
floor = 0.03
restart = 15
curve = 0.05:0.078, 0.2:0.14, 0.5:0.187, 0.8:0.203, 1:0.195
efficiency_grade = 0.02

# Little fuel goes through it, and it makes the most of it: but only near flat out.
[reactor deuterium_baseload]
name = Baseload deuterium fusion reactor
mass = 7.8
volume = 21
cost = 12000
material = Reactor Cores
fuel = Deuterium
burn = 0.018
floor = 0.36
restart = 80
curve = 0.3:0.22, 0.5:0.32, 0.75:0.43, 0.9:0.44, 1:0.43
efficiency_grade = 0.02

# Burns hard for its size; wasteful throttled back.
[reactor deuterium_high]
name = High-output deuterium fusion reactor
mass = 6.9
volume = 15
cost = 13500
material = Reactor Cores
fuel = Deuterium
burn = 0.0576
floor = 0.3
restart = 28
curve = 0.25:0.14, 0.5:0.24, 0.8:0.32, 1:0.35
efficiency_grade = 0.02

# Much the same efficiency at any load, and it idles on next to nothing.
[reactor deuterium_flat]
name = Load-following deuterium fusion reactor
mass = 6.6
volume = 15
cost = 9750
material = Reactor Cores
fuel = Deuterium
burn = 0.0312
floor = 0.06
restart = 24
curve = 0.06:0.31, 0.15:0.35, 1:0.35
efficiency_grade = 0.02

# Half the room and little more than half the mass, and it wastes more of its fuel.
[reactor deuterium_compact]
name = Compact deuterium fusion reactor
mass = 3.3
volume = 7.5
cost = 10500
material = Reactor Cores
fuel = Deuterium
burn = 0.0325
floor = 0.12
restart = 40
curve = 0.12:0.14, 0.3:0.218, 0.6:0.273, 0.85:0.296, 1:0.289
efficiency_grade = 0.02

# Little fuel goes through it, and it makes the most of it: but only near flat out.
[reactor helium3_baseload]
name = Baseload helium-3 fusor
mass = 4.68
volume = 21
cost = 28800
material = Reactor Cores
fuel = Helium-3
burn = 0.0101
floor = 0.45
restart = 120
curve = 0.45:0.6, 0.6:0.68, 0.8:0.74, 0.9:0.75, 1:0.74
efficiency_grade = 0.02

# Burns hard for its size; wasteful throttled back.
[reactor helium3_high]
name = High-output helium-3 fusor
mass = 4.14
volume = 15
cost = 32400
material = Reactor Cores
fuel = Helium-3
burn = 0.0405
floor = 0.375
restart = 42
curve = 0.375:0.42, 0.6:0.53, 0.8:0.59, 1:0.62
efficiency_grade = 0.02

# Much the same efficiency at any load, and it idles on next to nothing.
[reactor helium3_flat]
name = Load-following helium-3 fusor
mass = 3.96
volume = 15
cost = 23400
material = Reactor Cores
fuel = Helium-3
burn = 0.0225
floor = 0.075
restart = 36
curve = 0.075:0.55, 0.15:0.61, 1:0.61
efficiency_grade = 0.02

# Half the room and little more than half the mass, and it wastes more of its fuel.
[reactor helium3_compact]
name = Compact helium-3 fusor
mass = 1.98
volume = 7.5
cost = 25200
material = Reactor Cores
fuel = Helium-3
burn = 0.0225
floor = 0.15
restart = 60
curve = 0.15:0.42, 0.3:0.5, 0.6:0.58, 0.85:0.61, 1:0.6
efficiency_grade = 0.02

# --- Engines and thrusters ---------------------------------------------------
[engine standard]
name = Main engine
mass = 4
volume = 10
cost = 2500
material = Ship Components
thrust = 1312.5
draw = 160                     # terawatts, flat out

[thrusters standard]
name = Thrusters
mass = 1.5
volume = 4
cost = 1200
material = Ship Components
turn = 13750

# --- Drives ------------------------------------------------------------------
# Holding the field takes  field_base x size  +  field_mass x tonnes x (v/c)^field_power  terawatts.
# speed: top speed against the standard drive's (speed_grade: what each grade adds).
# max_mass: the heaviest ship it will move.
[hyperdrive standard]
name = Hyper drive
mass = 3
volume = 10
cost = 3000
material = Ship Components
speed = 1.0
speed_grade = 0.10
field_base = 80
field_mass = 2.112e-07
field_power = 3
accel = 120                  # c per second
max_mass = 225

# Cruising: the field takes well under half the power, and the drive is slower.
[hyperdrive cruise]
name = Cruising hyper drive
mass = 3.3
volume = 11
cost = 3900
material = Ship Components
speed = 0.75
speed_grade = 0.10
field_base = 60
field_mass = 8.4e-08
field_power = 3
accel = 100
max_mass = 225

# Fast: half as fast again, for a good deal more power at any speed.
[hyperdrive fast]
name = Fast hyper drive
mass = 3.6
volume = 12
cost = 6500
material = Ship Components
speed = 1.5
speed_grade = 0.10
field_base = 120
field_mass = 3.2e-07
field_power = 3
accel = 180
max_mass = 225

# Heavy-lift: moves twice the ship for its size, slowly. For hulls nothing else will shift.
[hyperdrive heavy]
name = Heavy-lift hyper drive
mass = 4.8
volume = 15
cost = 4500
material = Ship Components
speed = 0.8
speed_grade = 0.10
field_base = 110
field_mass = 2.4e-07
field_power = 3
accel = 80
max_mass = 450

# jump: terajoules a tonne of ship to form the bubble (jump_grade: the share each grade saves).
[warpdrive standard]
name = Warp drive
mass = 5
volume = 15
cost = 5000
material = Ship Components
speed = 1.0
speed_grade = 0.15
field_base = 240
field_mass = 2.208e-13
field_power = 2
accel = 3.0e6
max_mass = 225
jump = 800
jump_grade = 0.08

# The warp drives trade three things against each other: what the jump takes
# (jump), what holding the bubble takes under way (field_mass, and field_base),
# and how fast it goes.
#
# Cruising: under half the power to hold the bubble, at 70% of the speed. The
# jump costs what it always did.
[warpdrive cruise]
name = Cruising warp drive
mass = 5.5
volume = 16
cost = 6500
material = Ship Components
speed = 0.7
speed_grade = 0.15
field_base = 180
field_mass = 1.0e-13
field_power = 2
accel = 2.5e6
max_mass = 225
jump = 800
jump_grade = 0.08

# Short-haul: forms its bubble for well under half the energy, and then takes two and a
# half times the power to keep it up. For runs of a few light years, where the jump is the bill.
[warpdrive shorthaul]
name = Short-haul warp drive
mass = 6
volume = 18
cost = 8000
material = Ship Components
speed = 0.9
speed_grade = 0.15
field_base = 480
field_mass = 5.5e-13
field_power = 2
accel = 3.0e6
max_mass = 225
jump = 320
jump_grade = 0.08

# Deep-range: a bubble that all but keeps itself up, and takes a quarter as much again to form.
# For the long lanes, and for going where there aren't any.
[warpdrive deeprange]
name = Deep-range warp drive
mass = 6
volume = 18
cost = 9000
material = Ship Components
speed = 1.0
speed_grade = 0.15
field_base = 160
field_mass = 5.5e-14
field_power = 2
accel = 3.0e6
max_mass = 225
jump = 1000
jump_grade = 0.08

# Sprint: 60% faster and quick off the mark, and it pays for both in the jump and under way.
[warpdrive sprint]
name = Sprint warp drive
mass = 6.5
volume = 16
cost = 12000
material = Ship Components
speed = 1.6
speed_grade = 0.15
field_base = 360
field_mass = 2.9e-13
field_power = 2
accel = 6.0e6
max_mass = 225
jump = 960
jump_grade = 0.08

# Heavy-lift: moves twice the ship for its size, at four fifths of the speed.
[warpdrive heavy]
name = Heavy-lift warp drive
mass = 8
volume = 22
cost = 8500
material = Ship Components
speed = 0.8
speed_grade = 0.15
field_base = 300
field_mass = 2.4e-13
field_power = 2
accel = 2.0e6
max_mass = 450
jump = 800
jump_grade = 0.08

# --- Capacitors --------------------------------------------------------------
# The drive capacitor holds the charge for a jump: efficiency is what survives
# going in, and again coming out.
[drivecap standard]
name = Drive capacitor
mass = 2.5
volume = 12
cost = 1500
material = Electronics
capacity = 128000              # terajoules
efficiency = 0.90
efficiency_grade = 0.02

# Superconducting: loses next to nothing, holds less for its size, and costs.
[drivecap superconducting]
name = Superconducting drive capacitor
mass = 2.5
volume = 12
cost = 4500
material = Electronics
capacity = 80000
efficiency = 0.96
efficiency_grade = 0.01

# Bulk: twice the charge in the same room, heavier, and it leaks.
[drivecap bulk]
name = Bulk drive capacitor
mass = 4.5
volume = 12
cost = 1800
material = Electronics
capacity = 256000
efficiency = 0.84
efficiency_grade = 0.02

[bank standard]
name = Capacitor bank
mass = 1
volume = 5
cost = 800
material = Electronics
capacity = 19200

# --- Tanks, bays, cabins -----------------------------------------------------
[tank standard]
name = Fuel tank
mass = 0.5
volume = 12
cost = 300
material = Hull Plating
capacity = 10                # tonnes

[hold standard]
name = Cargo bay
mass = 1
volume = 30
cost = 400
material = Hull Plating
capacity = 25

# --- Working gear ------------------------------------------------------------
[mininglaser standard]
name = Mining laser
mass = 1.5
volume = 5
cost = 2000
material = Machinery
rate = 2                     # tonnes a second
range = 300
range_grade = 0.2
draw = 256

[scoop standard]
name = Fuel scoop
mass = 1
volume = 5
cost = 1500
material = Machinery
rate = 1

[skimmer standard]
name = Gas skimmer
mass = 1
volume = 5
cost = 1500
material = Machinery
rate = 0.25

# --- Sensors -----------------------------------------------------------------
# Each size reaches reach_size further than the smallest, each grade reach_grade further.
[telescope standard]
name = Telescope
mass = 0.3
volume = 3
cost = 1000
material = Instruments
reach_size = 0.5
reach_grade = 0.25
draw = 16

[radar standard]
name = Radar
mass = 0.5
volume = 3
cost = 1500
material = Electronics
reach_size = 0.5
reach_grade = 0.25
draw = 64                     # while it's on

[irarray standard]
name = Infrared array
mass = 0.3
volume = 2
cost = 1200
material = Instruments
reach_size = 0.5
reach_grade = 0.25
draw = 8

# A tachyon detector is a volume: twice the size is twice the volume, shared
# between its face (area, m2) and its depth (m).
[tachyondetector standard]
name = Tachyon detector
mass = 2
volume = 8
cost = 4000
material = Instruments
area = 4
depth = 2
catches = 0.5
catches_grade = 0.1
resolution = 12              # degrees, from one count
resolution_grade = 0.5
draw = 8

[cabins standard]
name = Passenger cabins
mass = 1.5
volume = 20
cost = 2500
material = Habitat Modules
capacity = 4                 # berths
draw = 6.4
)PARTS";
