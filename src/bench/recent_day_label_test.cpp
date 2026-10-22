#include "../ui/recent_day_label.h"
#include <cstdio>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    constexpr int kSun = 0, kMon = 1, kThu = 4, kSat = 6;
    check(ClassifyRecentDay(0, kThu, kMon) == RecentDayLabel::Today, "same day is today");
    check(ClassifyRecentDay(1, kThu, kMon) == RecentDayLabel::Yesterday, "one day back is yesterday");
    check(ClassifyRecentDay(2, kThu, kMon) == RecentDayLabel::ThisWeek &&
              ClassifyRecentDay(3, kThu, kMon) == RecentDayLabel::ThisWeek,
          "Thursday, Monday-first: Tuesday and Monday are this week");
    check(ClassifyRecentDay(4, kThu, kMon) == RecentDayLabel::LastWeek &&
              ClassifyRecentDay(6, kThu, kMon) == RecentDayLabel::LastWeek,
          "Thursday, Monday-first: Sunday back to Friday are last week");
    check(ClassifyRecentDay(7, kThu, kMon) == RecentDayLabel::Date &&
              ClassifyRecentDay(40, kThu, kMon) == RecentDayLabel::Date,
          "a week or more back shows the date");
    check(ClassifyRecentDay(-1, kThu, kMon) == RecentDayLabel::Date, "future times show the date");
    check(ClassifyRecentDay(2, kMon, kMon) == RecentDayLabel::LastWeek &&
              ClassifyRecentDay(6, kMon, kMon) == RecentDayLabel::LastWeek,
          "on the first day of the week every weekday label is last week");
    check(ClassifyRecentDay(6, kSun, kMon) == RecentDayLabel::ThisWeek,
          "Monday-first Sunday: the whole week so far is this week");
    check(ClassifyRecentDay(4, kThu, kSun) == RecentDayLabel::ThisWeek &&
              ClassifyRecentDay(5, kThu, kSun) == RecentDayLabel::LastWeek,
          "Sunday-first locales: Sunday is this week, Saturday last week");
    check(ClassifyRecentDay(6, kSat, kSun) == RecentDayLabel::ThisWeek,
          "Sunday-first Saturday: six days back is still this week");
    return failures ? 1 : 0;
}
