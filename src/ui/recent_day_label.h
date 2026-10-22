#pragma once

// Which relative label the file list's smart dates use for a day `days_ago`
// before today. Weekday names only stay unambiguous inside the current week:
// on a Thursday, a bare "Fri" would read as tomorrow, so the days before the
// week began are "last Fri" instead.
namespace pulse::ui {

enum class RecentDayLabel { Today, Yesterday, ThisWeek, LastWeek, Date };

// `today_dow` and `first_dow` count from Sunday = 0 (SYSTEMTIME::wDayOfWeek);
// `first_dow` is the locale's first day of the week.
constexpr RecentDayLabel ClassifyRecentDay(long long days_ago, int today_dow, int first_dow) {
    if (days_ago == 0) return RecentDayLabel::Today;
    if (days_ago == 1) return RecentDayLabel::Yesterday;
    if (days_ago < 0 || days_ago >= 7) return RecentDayLabel::Date;
    const int into_week = ((today_dow - first_dow) % 7 + 7) % 7;  // days since the week began
    return days_ago <= into_week ? RecentDayLabel::ThisWeek : RecentDayLabel::LastWeek;
}

}  // namespace pulse::ui
