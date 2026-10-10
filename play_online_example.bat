@echo off
rem For testers: put this next to freehauler.exe, change the address and name, double-click.
rem Ask whoever runs the server for its address (and password, if it has one).
freehauler.exe --connect 203.0.113.7:23500 --name "Your Name"
rem With a password:   freehauler.exe --connect 203.0.113.7:23500 --name "Your Name" --password secret
