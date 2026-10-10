#include "format.h"
#include "core.h"

std::string FormatSpeed(double uPerSec)
{
    double ms = uPerSec * METERS_PER_UNIT;
    if (ms < 1000.0)                    return TextFormat("%.0f m/s", ms);
    if (uPerSec < 0.001 * AU_U)         return TextFormat("%.1f km/s", ms / 1000.0);
    if (uPerSec < 1000.0 * AU_U)        return TextFormat("%.3f AU/s", uPerSec / AU_U);
    return TextFormat("%.3f ly/s", uPerSec / LY_U);
}

std::string FormatPlanetDist(double u)
{
    if (u < 1.0 * KM_U)   return TextFormat("%.0f m", u * METERS_PER_UNIT);
    if (u < 10.0 * KM_U)  return TextFormat("%.1f km", u / KM_U);
    if (u < 0.01 * AU_U)  return TextFormat("%.0f km", u / KM_U);
    return TextFormat("%.2f AU", u / AU_U);
}

std::string FormatStarDist(double u)
{
    if (u < 0.01 * LY_U) return TextFormat("%.2f AU", u / AU_U);
    return TextFormat("%.2f ly", u / LY_U);
}

std::string FormatETA(double seconds)
{
    if (seconds < 60.0)            return TextFormat("%.0f s", seconds);
    long s = (long)seconds;
    if (seconds < 3600.0)          return TextFormat("%ldm %02lds", s / 60, s % 60);
    if (seconds < 86400.0)         return TextFormat("%ldh %02ldm", s / 3600, (s / 60) % 60);
    if (seconds < 365.25 * 86400)  return TextFormat("%ldd %02ldh", s / 86400, (s / 3600) % 24);
    return TextFormat("%.1f years", seconds / (365.25 * 86400.0));
}

std::string FormatPeriod(double seconds)
{
    double days = seconds / 86400.0;
    if (days < 2.0)     return TextFormat("%.1f hours", seconds / 3600.0);
    if (days < 730.0)   return TextFormat("%.1f days", days);
    return TextFormat("%.1f years", days / 365.25);
}

std::string Thousands(double v)
{
    std::string s = TextFormat("%.0f", v);
    int start = (!s.empty() && s[0] == '-') ? 1 : 0;
    for (int i = (int)s.size() - 3; i > start; i -= 3) s.insert(i, ",");
    return s;
}

std::string FormatPopulation(double p)
{
    if (p >= 1e9) return TextFormat("%.1f billion", p / 1e9);
    if (p >= 1e6) return TextFormat("%.0f million", p / 1e6);
    return Thousands(p);
}

std::string FormatTemp(double k) { return TextFormat("%.0f K  (%.0f C)", k, k - 273.15); }
