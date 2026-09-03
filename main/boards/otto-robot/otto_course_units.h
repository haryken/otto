#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>

struct OttoUnitList {
    const char* name;
    const char* const* units;
    int unit_count;
};

struct OttoCourseDef {
    const char* id;
    const char* display_name;
    const char* const* units;
    int unit_count;
    const OttoUnitList* subs;
    int sub_count;
};

/* Explorers — 5 sách con */
inline constexpr const char* kUnitsPreDiscoveryA[] = {
    "Unit Routines",
    "Unit Welcome The Magic Forest",
    "Unit One Let's Draw",
    "Unit Two Let's Play",
    "Unit Review One",
    "Unit Three The Big Monster",
    "Unit Special Day One",
    "Unit Special Day Two",
};

inline constexpr const char* kUnitsPreDiscoveryB[] = {
    "Unit Routines Review",
    "Unit Four My Family",
    "Unit Review Two",
    "Unit Five Where's My Bird?",
    "Unit Midterm Test",
    "Unit Six Let's Tidy Up!",
    "Unit Review Three",
    "Unit Special Day Three",
    "Unit Special Day Four",
    "Unit Review Four",
    "Unit Final Test",
};

inline constexpr const char* kUnitsPreSparkA[] = {
    "Unit Routines",
    "Unit Welcome Let's Remember",
    "Unit One The Surprise",
    "Unit Two The Brown Mouse",
    "Unit Review One",
    "Unit Three Where Is Greenman?",
    "Unit Special Day One",
    "Unit Special Day Two",
};

inline constexpr const char* kUnitsSparkTwoA[] = {
    "Unit Our Friends",
    "Unit One Weather",
    "Unit Two Our Families",
    "Unit Three Our Places",
    "Unit Four Our Colors and Shapes",
};

inline constexpr const char* kUnitsSparkThreeA[] = {
    "Unit Hello Again",
    "Unit One Our Day",
    "Unit Two Dinner Time",
    "Unit Three With My Friends",
    "Unit Four Our Colors and Shapes",
    "Unit Four Our Faces",
};

inline constexpr OttoUnitList kExplorersSubs[] = {
    {"Pre-Discovery A", kUnitsPreDiscoveryA, 8},
    {"Pre-Discovery B", kUnitsPreDiscoveryB, 11},
    {"PRE-SPARK A", kUnitsPreSparkA, 8},
    {"SPARK Two A", kUnitsSparkTwoA, 5},
    {"SPARK Three A", kUnitsSparkThreeA, 6},
};

/* Young Innovators — 3 sách QUEST */
inline constexpr const char* kUnitsQuestOneA[] = {
    "Unit One Hello",
    "Unit Two My School",
    "Unit Three Favourite Toys",
    "Unit Four My Family",
    "Unit Five Our Pets",
    "Unit Six My Face",
};

inline constexpr const char* kUnitsQuestTwoB[] = {
    "Unit Seven At the Farm",
    "Unit Eight My Town",
    "Unit Nine Our Clothes",
    "Unit Ten Our Hobbies",
    "Unit Eleven My Birthday",
    "Unit Twelve Our Holiday",
};

inline constexpr const char* kUnitsQuestThreeA[] = {
    "Unit Hello",
    "Unit One Family Matters",
    "Unit Two Home Sweet Home",
    "Unit Values One and Two",
    "Unit Three A Day in the Life",
    "Unit Four In the City",
    "Unit Values Three and Four",
    "Unit YLE Movers Listening Skills Practice",
};

inline constexpr OttoUnitList kYoungInnovatorsSubs[] = {
    {"QUEST One A", kUnitsQuestOneA, 6},
    {"QUEST Two B", kUnitsQuestTwoB, 6},
    {"QUEST Three A", kUnitsQuestThreeA, 8},
};

/* Future Leaders — sách Summit */
inline constexpr const char* kUnitsSummitOneA[] = {
    "Unit Welcome",
    "Unit One Having a Good Time",
    "Unit Two Spending Money",
    "Unit Three We Are What We Eat",
    "Unit Four All in the Family",
    "Unit Five No Place Like Home",
    "Unit Six Friends Forever",
};

inline constexpr OttoUnitList kFutureLeadersSubs[] = {
    {"Summit One A", kUnitsSummitOneA, 7},
};

/* Các khóa còn lại — mock tạm */
inline constexpr const char* kUnitsMock[] = {
    "Unit 1",
    "Unit 2",
    "Unit 3",
    "Unit 4",
    "Unit 5",
    "Unit 6",
};

inline constexpr int kExplorersCourseIdx = 1;
inline constexpr int kYoungInnovatorsCourseIdx = 2;
inline constexpr int kFutureLeadersCourseIdx = 3;
inline constexpr int kManualMacCourseIdx = 6;
inline constexpr int kDailyChatCourseIdx = 7;

inline constexpr OttoCourseDef kOttoCourses[] = {
    {"custom", "Tự cấu hình", nullptr, 0, nullptr, 0},
    {"explorers", "Explorers", nullptr, 0, kExplorersSubs, 5},
    {"younginnovators", "Young Innovators", nullptr, 0, kYoungInnovatorsSubs, 3},
    {"futureleaders", "Future Leaders", nullptr, 0, kFutureLeadersSubs, 1},
    {"ielts", "IELTS", kUnitsMock, 6, nullptr, 0},
    {"toeic", "TOEIC", kUnitsMock, 6, nullptr, 0},
    {"manual_mac", "Tự nhập MAC", nullptr, 0, nullptr, 0},
    {"daily_chat", "Giao tiếp hằng ngày", nullptr, 0, nullptr, 0},
};

inline const OttoCourseDef* GetOttoCourse(int idx) {
    if (idx < 0 || idx > 7) {
        return nullptr;
    }
    return &kOttoCourses[idx];
}

inline const OttoUnitList* GetOttoActiveUnitList(int course_idx, int sub_idx) {
    const OttoCourseDef* course = GetOttoCourse(course_idx);
    if (course == nullptr) {
        return nullptr;
    }
    if (course->sub_count > 0) {
        if (sub_idx < 0 || sub_idx >= course->sub_count) {
            sub_idx = 0;
        }
        return &course->subs[sub_idx];
    }
    static OttoUnitList flat;
    if (course->unit_count <= 0) {
        return nullptr;
    }
    flat.name = course->display_name;
    flat.units = course->units;
    flat.unit_count = course->unit_count;
    return &flat;
}

inline std::string ResolveOttoUnitNames(int course_idx, int sub_idx, const std::string& selected) {
    const OttoUnitList* list = GetOttoActiveUnitList(course_idx, sub_idx);
    if (list == nullptr || list->unit_count <= 0) {
        return selected;
    }
    int n = atoi(selected.c_str());
    if (n >= 1 && n <= list->unit_count) {
        return std::string(list->units[n - 1]);
    }
    return selected;
}
