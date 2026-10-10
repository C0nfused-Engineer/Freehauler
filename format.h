// format.h - turning numbers into HUD text
#pragma once
#include <string>

std::string FormatSpeed(double uPerSec);       // m/s, km/s, AU/s or ly/s
std::string FormatPlanetDist(double u);        // AU, or km / m when close
std::string FormatStarDist(double u);          // ly, or AU inside a system
std::string FormatETA(double seconds);
std::string FormatPeriod(double seconds);      // hours / days / years
std::string Thousands(double v);               // 58179 -> "58,179"
std::string FormatPopulation(double p);
std::string FormatTemp(double kelvin);
