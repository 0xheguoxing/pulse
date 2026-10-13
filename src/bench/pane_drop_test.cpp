// pane_drop_test.cpp - Multi-window M7h: a tab split in panes dropped on an
// edge of another window's content brings all its panes, up to four in all
// (the prototype's addSplit), in their order, with a preview per pane.
// M7i: the pane under the cursor splits at its own edge. M7k: exactly that
// pane splits in half (a free split tree) and the other panes stay put.
#include "../app/pane_drop.h"
#include "../app/pane_layout.h"
#include "../app/session.h"
#include "../app/split_tree_shape.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace pulse::app;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// A tab of `count` visible panes, side by side as drops at the right make it.
std::unique_ptr<LayoutTab> SplitTab(size_t count) {
    auto tab = MakeSingleLayoutTab(L"");
    for (size_t i = 1; i < count; ++i) AddPaneFromTab(*tab, MakeSingleLayoutTab(L""), ContentDrop::Right);
    return tab;
}

bool LeftHalf(const D2D1_RECT_F& r) { return r.left >= 0.0f && r.right <= 500.0f; }
bool RightHalf(const D2D1_RECT_F& r) { return r.left >= 500.0f && r.right <= 1000.0f; }
bool TopHalf(const D2D1_RECT_F& r) { return r.top >= 0.0f && r.bottom <= 400.0f; }
bool BottomHalf(const D2D1_RECT_F& r) { return r.top >= 400.0f && r.bottom <= 800.0f; }

bool Same(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return std::fabs(a.left - b.left) < 0.5f && std::fabs(a.top - b.top) < 0.5f && std::fabs(a.right - b.right) < 0.5f &&
           std::fabs(a.bottom - b.bottom) < 0.5f;
}

// Where `pane` is in `content`.
D2D1_RECT_F RectOf(const LayoutTab& tab, const Pane* pane, const D2D1_RECT_F& content) {
    std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
    if (tab.root) LayoutWindowPanes(*tab.root, content, 1.0f, laid);
    for (const auto& [p, r] : laid)
        if (p == pane) return r;
    return {};
}

// True when `r` is the half of `pane` at `side`: it shares the three outer
// edges, and reaches about the middle (the splitter gap aside).
bool HalfOf(const D2D1_RECT_F& r, const D2D1_RECT_F& pane, ContentDrop side) {
    const float w = pane.right - pane.left, h = pane.bottom - pane.top, slack = 8.0f;
    const auto at = [&](float a, float b) { return std::fabs(a - b) < 0.5f; };
    const auto mid = [&](float a, float m) { return std::fabs(a - m) <= slack; };
    switch (side) {
    case ContentDrop::Left:
        return at(r.left, pane.left) && at(r.top, pane.top) && at(r.bottom, pane.bottom) &&
               mid(r.right, pane.left + w * 0.5f);
    case ContentDrop::Right:
        return at(r.right, pane.right) && at(r.top, pane.top) && at(r.bottom, pane.bottom) &&
               mid(r.left, pane.left + w * 0.5f);
    case ContentDrop::Top:
        return at(r.top, pane.top) && at(r.left, pane.left) && at(r.right, pane.right) &&
               mid(r.bottom, pane.top + h * 0.5f);
    case ContentDrop::Bottom:
        return at(r.bottom, pane.bottom) && at(r.left, pane.left) && at(r.right, pane.right) &&
               mid(r.top, pane.top + h * 0.5f);
    default: return false;
    }
}

}  // namespace

int main() {
    const D2D1_RECT_F content{0.0f, 0.0f, 1000.0f, 800.0f};

    Check(GrownPreset(1, 2, ContentDrop::Left) == LayoutPreset::Three &&
              GrownPreset(2, 2, ContentDrop::Top) == LayoutPreset::FourGrid &&
              GrownPreset(1, 3, ContentDrop::Bottom) == LayoutPreset::FourGrid &&
              GrownPreset(1, 1, ContentDrop::Top) == LayoutPreset::TwoHorizontal &&
              GrownPreset(2, 3, ContentDrop::Right) == LayoutPreset::Single &&
              GrownPreset(1, 2, ContentDrop::Center) == LayoutPreset::Single &&
              GrownPreset(0, 2, ContentDrop::Left) == LayoutPreset::Single,
          "the grown layout counts every pane brought: 3 in all -> three, 4 -> grid, more -> none");
    Check(GrownPreset(1, ContentDrop::Left) == GrownPreset(1, 1, ContentDrop::Left) &&
              GrownPreset(3, ContentDrop::Right) == LayoutPreset::FourGrid,
          "the one-pane form is unchanged (M6d)");

    {
        auto host = MakeSingleLayoutTab(L"");
        Pane* const own = host->panes[0].get();
        auto incoming = SplitTab(2);
        const std::vector<Pane*> brought = incoming->VisiblePanes();
        incoming->focused_index = 0;
        Check(DropPaneCount(*incoming) == 2 && HasRoomForPanes(*host, *incoming) &&
                  CanAddPane(*host, *incoming, ContentDrop::Left) && !CanAddPane(*host, *incoming, ContentDrop::Center),
              "a two-pane tab fits next to one pane, at an edge only");
        const std::vector<D2D1_RECT_F> rects = PaneDropRects(*host, 2, ContentDrop::Right, content, 1.0f);
        Check(rects.size() == 2 && RightHalf(rects[0]) && RightHalf(rects[1]) && TopHalf(rects[0]) &&
                  BottomHalf(rects[1]),
              "the preview shows each pane it brings: right column, first on top");
        const bool added = AddPaneFromTab(*host, std::move(incoming), ContentDrop::Right);
        Check(added && host->layout == LayoutPreset::Three && host->panes.size() == 3 &&
                  host->VisiblePanes() == std::vector<Pane*>{own, brought[0], brought[1]},
              "dropped on the right edge its panes come last, in their order");
        Check(host->FocusedPane() == brought[0] && brought[0]->focused && !brought[1]->focused && !own->focused,
              "the pane that was focused in the dragged tab is focused");
    }

    {
        auto host = MakeSingleLayoutTab(L"");
        Pane* const own = host->panes[0].get();
        auto incoming = SplitTab(2);
        const std::vector<Pane*> brought = incoming->VisiblePanes();
        const std::vector<D2D1_RECT_F> rects = PaneDropRects(*host, 2, ContentDrop::Left, content, 1.0f);
        Check(rects.size() == 2 && LeftHalf(rects[0]) && rects[0].bottom >= 799.0f && RightHalf(rects[1]) &&
                  TopHalf(rects[1]),
              "at the left edge the first brought pane takes the big left one (Three)");
        Check(AddPaneFromTab(*host, std::move(incoming), ContentDrop::Left) &&
                  host->VisiblePanes() == std::vector<Pane*>{brought[0], brought[1], own},
              "dropped on the left edge its panes come first, in their order");
    }

    {
        auto host = SplitTab(2);
        const std::vector<Pane*> mine = host->VisiblePanes();
        auto incoming = SplitTab(2);
        const std::vector<Pane*> brought = incoming->VisiblePanes();
        const std::vector<D2D1_RECT_F> rects = PaneDropRects(*host, 2, ContentDrop::Left, content, 1.0f);
        Check(rects.size() == 2 && LeftHalf(rects[0]) && LeftHalf(rects[1]) && TopHalf(rects[0]) &&
                  BottomHalf(rects[1]),
              "two joining two at the left edge preview the left column");
        Check(AddPaneFromTab(*host, std::move(incoming), ContentDrop::Left) && host->layout == LayoutPreset::FourGrid &&
                  host->VisiblePanes() == std::vector<Pane*>{brought[0], mine[0], brought[1], mine[1]},
              "and take the left column of the grid");
    }

    {
        auto host = SplitTab(2);
        const std::vector<Pane*> mine = host->VisiblePanes();
        auto incoming = SplitTab(2);
        const std::vector<Pane*> brought = incoming->VisiblePanes();
        const std::vector<D2D1_RECT_F> right = PaneDropRects(*host, 2, ContentDrop::Right, content, 1.0f);
        Check(right.size() == 2 && RightHalf(right[0]) && RightHalf(right[1]) && TopHalf(right[0]) &&
                  BottomHalf(right[1]),
              "two joining two at the right edge preview the right column");
        const std::vector<D2D1_RECT_F> top = PaneDropRects(*host, 2, ContentDrop::Top, content, 1.0f);
        Check(top.size() == 2 && TopHalf(top[0]) && TopHalf(top[1]) && LeftHalf(top[0]) && RightHalf(top[1]),
              "at the top edge they preview the top row");
        host->target_index = 0;  // mine[0] is the copy/move target
        mine[0]->target = true;
        brought[0]->target = true;
        incoming->target_index = 0;
        Check(AddPaneFromTab(*host, std::move(incoming), ContentDrop::Bottom) &&
                  host->VisiblePanes() == std::vector<Pane*>{mine[0], mine[1], brought[0], brought[1]},
              "at the bottom edge they take the bottom row");
        Check(host->target_index >= 0 && host->panes[static_cast<size_t>(host->target_index)].get() == mine[0] &&
                  !brought[0]->target,
              "the copy/move target stays where it was; a brought pane is no target");
    }

    {
        auto host = SplitTab(3);
        auto incoming = SplitTab(2);
        const LayoutTab* const raw = incoming.get();
        Check(!HasRoomForPanes(*host, *incoming) && !CanAddPane(*host, *incoming, ContentDrop::Left) &&
                  !AddPaneFromTab(*host, std::move(incoming), ContentDrop::Left) && host->VisiblePanes().size() == 3,
              "three panes and two more would be five: no split (the hint says the tab is full)");
        (void)raw;
        auto two = SplitTab(2);
        auto pair = SplitTab(2);
        Check(HasRoomForPanes(*two, *pair) && !HasRoomForPanes(*SplitTab(4), *MakeSingleLayoutTab(L"")),
              "two and two fit; four take nothing more");
        auto host3 = MakeSingleLayoutTab(L"");
        auto three = SplitTab(3);
        const std::vector<Pane*> brought = three->VisiblePanes();
        const std::vector<D2D1_RECT_F> rects = PaneDropRects(*host3, 3, ContentDrop::Right, content, 1.0f);
        Check(rects.size() == 3 && AddPaneFromTab(*host3, std::move(three), ContentDrop::Right) &&
                  host3->layout == LayoutPreset::FourGrid && host3->VisiblePanes()[1] == brought[0] &&
                  host3->VisiblePanes()[3] == brought[2],
              "a three-pane tab next to one pane makes the grid, in order");
    }

    {
        auto hidden = MakeSingleLayoutTab(L"");
        hidden->panes.push_back(std::make_unique<Pane>());  // kept but not shown
        auto host = MakeSingleLayoutTab(L"");
        Check(DropPaneCount(*hidden) == 0 && !HasRoomForPanes(*host, *hidden) &&
                  !CanAddPane(*host, *hidden, ContentDrop::Left),
              "a tab that keeps hidden panes can only become a tab");
        LayoutTab empty;
        Check(DropPaneCount(empty) == 0 && !CanAddPane(*host, empty, ContentDrop::Left),
              "a tab without panes brings nothing");
    }

    {
        auto host = MakeSingleLayoutTab(L"");
        Check(PaneDropRect(*host, ContentDrop::Left, content, 1.0f).left == 0.0f &&
                  PaneDropRect(*host, ContentDrop::Left, content, 1.0f).right <= 500.0f &&
                  PaneDropRects(*host, 1, ContentDrop::Center, content, 1.0f).size() == 1 &&
                  PaneDropRects(*host, 1, ContentDrop::Center, content, 1.0f)[0].right == 1000.0f,
              "one pane still previews its half; no split previews the whole content");
    }

    // M7k: the split tree. Shapes, presets, and keeping a custom tree.
    {
        const std::vector<Pane*> none(4, nullptr);
        Check(SplitTreeShape(*MakePresetTree(LayoutPreset::Three, none)) == L"VPHPP" &&
                  SplitTreeShape(*MakePresetTree(LayoutPreset::FourGrid, none)) == L"HVPPVPP" &&
                  SplitTreeShape(*MakePresetTree(LayoutPreset::Single, none)) == L"P",
              "a tree's shape lists its splits and panes in tree order");
        Check(ShapePaneCount(L"VPHPP") == 3 && ShapePaneCount(L"P") == 1 && ShapePaneCount(L"VP") == 0 &&
                  ShapePaneCount(L"PP") == 0 && ShapePaneCount(L"VPX") == 0 && ShapePaneCount(L"") == 0,
              "a malformed shape holds no panes");
        auto tab = SplitTab(3);
        const std::vector<Pane*> three = tab->VisiblePanes();
        auto shaped = MakeShapedTree(L"VHPPP", three);
        std::vector<Pane*> order;
        if (shaped) shaped->CollectPanes(order);
        Check(shaped && SplitTreeShape(*shaped) == L"VHPPP" && order == three && PresetOfTree(*shaped) == LayoutPreset::Custom &&
                  !MakeShapedTree(L"VPP", three) && !MakeShapedTree(L"VPX", three),
              "a shape builds its tree with the panes in order, only when they fit");
        bool named = true;
        for (const LayoutPreset preset : {LayoutPreset::Single, LayoutPreset::TwoVertical, LayoutPreset::TwoHorizontal,
                                          LayoutPreset::Three, LayoutPreset::FourGrid})
            named = named && PresetOfTree(*MakePresetTree(preset, none)) == preset;
        Check(named &&
                  PresetForPaneCount(1) == LayoutPreset::Single && PresetForPaneCount(2) == LayoutPreset::TwoVertical &&
                  PresetForPaneCount(3) == LayoutPreset::Three && PresetForPaneCount(4) == LayoutPreset::FourGrid,
              "a preset's tree is named by it; a pane count names the nearest preset");
        tab->root = std::move(shaped);
        tab->layout = LayoutPreset::Custom;
        RebuildLayoutRoot(*tab);
        Check(tab->layout == LayoutPreset::Custom && tab->root && SplitTreeShape(*tab->root) == L"VHPPP",
              "rebuilding keeps a custom tree whose panes are the tab's");
        tab->root.reset();
        RebuildLayoutRoot(*tab);
        Check(tab->layout == LayoutPreset::Three && tab->root && SplitTreeShape(*tab->root) == L"VPHPP",
              "a custom tab without its tree takes the preset with as many panes");
        Check(LayoutTabTitle(*tab).find(L"3") != std::wstring::npos, "the tab title counts its panes");
    }

    // M7k: aim at the pane under the cursor; exactly it splits in half.
    {
        auto host = SplitTab(2);  // left | right: the reported case
        const std::vector<Pane*> mine = host->VisiblePanes();
        const D2D1_RECT_F left = RectOf(*host, mine[0], content), right = RectOf(*host, mine[1], content);
        ContentDrop side = ContentDrop::None;
        const PanePlacement high = AimPaneDrop(*host, 1, content, 1.0f, POINT{250, 60}, &side);
        const std::vector<D2D1_RECT_F> rects = PlacementRects(*host, high, content, 1.0f);
        Check(side == ContentDrop::Top && high.pane == mine[0] && high.layout == LayoutPreset::Custom && rects.size() == 1 &&
                  HalfOf(rects[0], left, ContentDrop::Top),
              "2 panes: the top of the left pane previews the top half of that pane, not the whole column");
        auto incoming = MakeSingleLayoutTab(L"");
        Pane* const dropped = incoming->panes[0].get();
        Check(AddPaneFromTab(*host, std::move(incoming), high) && host->layout == LayoutPreset::Custom &&
                  SplitTreeShape(*host->root) == L"VHPPP" &&
                  host->VisiblePanes() == std::vector<Pane*>{dropped, mine[0], mine[1]} && host->FocusedPane() == dropped,
              "the new pane goes above the left one: a custom tree");
        Check(Same(RectOf(*host, mine[1], content), right) && Same(RectOf(*host, dropped, content), rects[0]) &&
                  HalfOf(RectOf(*host, mine[0], content), left, ContentDrop::Bottom),
              "the right pane stays put; the new pane is where the preview was");
    }
    {
        auto host = SplitTab(2);
        const std::vector<Pane*> mine = host->VisiblePanes();
        const PanePlacement low = AimPaneDrop(*host, 1, content, 1.0f, POINT{750, 760});
        const std::vector<D2D1_RECT_F> rects = PlacementRects(*host, low, content, 1.0f);
        Check(rects.size() == 1 && RightHalf(rects[0]) && BottomHalf(rects[0]) &&
                  AddPaneFromTab(*host, MakeSingleLayoutTab(L""), low) && host->layout == LayoutPreset::Three &&
                  host->VisiblePanes()[0] == mine[0] && host->VisiblePanes()[1] == mine[1],
              "2 panes: under the right pane is the three-pane preset after all, named so");
    }
    {
        auto host = SplitTab(3);  // Three: a big left pane, two stacked on the right
        const std::vector<Pane*> before = host->VisiblePanes();
        const D2D1_RECT_F left = RectOf(*host, before[0], content);
        ContentDrop side = ContentDrop::None;
        const PanePlacement low = AimPaneDrop(*host, 1, content, 1.0f, POINT{250, 700}, &side);
        const std::vector<D2D1_RECT_F> rects = PlacementRects(*host, low, content, 1.0f);
        Check(side == ContentDrop::Bottom && low.pane == before[0] && rects.size() == 1 &&
                  HalfOf(rects[0], left, ContentDrop::Bottom),
              "3 panes: the lower part of the left pane previews its bottom half");
        auto incoming = MakeSingleLayoutTab(L"");
        Pane* const dropped = incoming->panes[0].get();
        Check(AddPaneFromTab(*host, std::move(incoming), low) && host->layout == LayoutPreset::Custom &&
                  host->VisiblePanes() == std::vector<Pane*>{before[0], dropped, before[1], before[2]},
              "the new pane goes under the left one; the right ones keep their places");
        Check(!AimPaneDrop(*host, 1, content, 1.0f, POINT{100, 100}).Splits(),
              "a tab with four panes has no room to aim at");
    }
    {
        auto host = MakeSingleLayoutTab(L"");
        Pane* const own = host->panes[0].get();
        ContentDrop side = ContentDrop::None;
        const PanePlacement at_left = AimPaneDrop(*host, 1, content, 1.0f, POINT{100, 400}, &side);
        Check(side == ContentDrop::Left && at_left.pane == own &&
                  AimPaneDrop(*host, 1, content, 1.0f, POINT{620, 400}, &side).side == ContentDrop::Right &&
                  AimPaneDrop(*host, 1, content, 1.0f, POINT{500, 300}, &side).side == ContentDrop::Top,
              "one pane: its diagonals pick the side (M7j)");
        auto incoming = MakeSingleLayoutTab(L"");
        Pane* const dropped = incoming->panes[0].get();
        Check(AddPaneFromTab(*host, std::move(incoming), at_left) && host->layout == LayoutPreset::TwoVertical &&
                  host->VisiblePanes() == std::vector<Pane*>{dropped, own},
              "one pane split at its left is the side-by-side preset");
        side = ContentDrop::Center;
        Check(!AimPaneDrop(*host, 1, content, 1.0f, POINT{1200, 400}, &side).Splits() && side == ContentDrop::None,
              "outside the content there is no drop");
        const PanePlacement gap = AimPaneDrop(*host, 1, content, 1.0f, POINT{500, 700}, &side);
        Check(gap.Splits() && (gap.pane == own || gap.pane == dropped), "a cursor on a splitter aims at the nearest pane");
    }
    {
        auto host = MakeSingleLayoutTab(L"");
        Pane* const own = host->panes[0].get();
        auto pair = SplitTab(2);  // side by side
        const std::vector<Pane*> brought = pair->VisiblePanes();
        const PanePlacement right = AimPaneDrop(*host, 2, content, 1.0f, POINT{900, 400});
        const std::vector<D2D1_RECT_F> rects = PlacementRects(*host, right, content, 1.0f, pair.get());
        Check(rects.size() == 2 && RightHalf(rects[0]) && RightHalf(rects[1]) && rects[0].right <= rects[1].left &&
                  rects[0].bottom >= 799.0f,
              "a two-pane tab previews its own arrangement in the half it takes");
        Check(AddPaneFromTab(*host, std::move(pair), right) &&
                  host->VisiblePanes() == std::vector<Pane*>{own, brought[0], brought[1]} &&
                  Same(RectOf(*host, brought[1], content), rects[1]),
              "and keeps it there");
        PanePlacement stranger;
        stranger.layout = LayoutPreset::Custom;
        stranger.pane = brought[0] + 1;  // no pane of this tab
        stranger.side = ContentDrop::Left;
        PanePlacement bad{LayoutPreset::FourGrid, {1, 2}};
        Check(!CanPlacePanes(*host, *MakeSingleLayoutTab(L""), stranger) &&
                  !AddPaneFromTab(*host, MakeSingleLayoutTab(L""), stranger) &&
                  !CanPlacePanes(*host, *MakeSingleLayoutTab(L""), bad) && host->VisiblePanes().size() == 3,
              "a placement that does not fit the tab changes nothing");
    }

    // M7k: anywhere on any layout, the preview is half of the aimed pane, the
    // drop lands there, and every other pane stays where it was.
    {
        const auto custom = [] {
            auto tab = SplitTab(2);
            AddPaneFromTab(*tab, MakeSingleLayoutTab(L""), AimPaneDrop(*tab, 1, D2D1_RECT_F{0, 0, 1000, 800}, 1.0f,
                                                                      POINT{250, 60}));
            return tab;
        };
        int cases = 0, bad_preview = 0, bad_result = 0;
        for (int kind = 0; kind < 4; ++kind) {
            for (int gx = 0; gx < 9; ++gx) {
                for (int gy = 0; gy < 7; ++gy) {
                    auto host = kind == 3 ? custom() : SplitTab(static_cast<size_t>(kind) + 1);
                    const POINT pt{25 + gx * 118, 20 + gy * 126};
                    ContentDrop side = ContentDrop::None;
                    const PanePlacement aim = AimPaneDrop(*host, 1, content, 1.0f, pt, &side);
                    if (!aim.Splits()) {
                        ++bad_preview;
                        continue;
                    }
                    ++cases;
                    const std::vector<Pane*> was = host->VisiblePanes();
                    std::vector<D2D1_RECT_F> old_rects;
                    for (Pane* pane : was) old_rects.push_back(RectOf(*host, pane, content));
                    const D2D1_RECT_F aimed = RectOf(*host, aim.pane, content);
                    const std::vector<D2D1_RECT_F> preview = PlacementRects(*host, aim, content, 1.0f);
                    if (preview.size() != 1 || !HalfOf(preview[0], aimed, side)) ++bad_preview;
                    auto incoming = MakeSingleLayoutTab(L"");
                    Pane* const dropped = incoming->panes[0].get();
                    bool ok = AddPaneFromTab(*host, std::move(incoming), aim) && preview.size() == 1 &&
                              Same(RectOf(*host, dropped, content), preview[0]);
                    for (size_t i = 0; ok && i < was.size(); ++i)
                        if (was[i] != aim.pane && !Same(RectOf(*host, was[i], content), old_rects[i])) ok = false;
                    if (!ok) ++bad_result;
                }
            }
        }
        std::printf("  %d aimed drops\n", cases);
        Check(cases == 4 * 9 * 7 && bad_preview == 0,
              "1, 2, 3 panes and a custom tree: every spot previews half of the pane under it");
        Check(bad_result == 0, "the drop lands on the preview and no other pane moves");
    }

    // M7k: a custom tree survives the session file.
    {
        auto tab = SplitTab(2);
        AddPaneFromTab(*tab, MakeSingleLayoutTab(L""), AimPaneDrop(*tab, 1, content, 1.0f, POINT{250, 60}));
        std::vector<float> ratios;
        CollectSplitRatios(*tab->root, ratios);
        for (size_t i = 0; i < ratios.size(); ++i) ratios[i] = 0.35f + 0.1f * static_cast<float>(i);
        ApplySplitRatios(*tab->root, ratios);
        const LayoutTabSnapshot saved = CaptureLayoutTab(*tab);
        Check(saved.split_tree == L"VHPPP" && saved.layout == static_cast<int>(LayoutPreset::Three) &&
                  saved.panes.size() == 3,
              "a custom tree is saved by its shape, with the three-pane preset for older versions");
        std::vector<LayoutTabSnapshot> parsed;
        Check(ParseLayoutTabs(LayoutTabsToJson({saved}), parsed) && parsed.size() == 1 &&
                  parsed[0].split_tree == L"VHPPP",
              "the shape goes through the session JSON");
        LayoutTab back;
        RestoreLayoutTab(back, parsed.empty() ? saved : parsed[0], {});
        RebuildLayoutRoot(back);
        if (back.root) ApplySplitRatios(*back.root, parsed.empty() ? saved.split_ratios : parsed[0].split_ratios);
        std::vector<float> got;
        if (back.root) CollectSplitRatios(*back.root, got);
        bool close = got.size() == ratios.size();
        for (size_t i = 0; close && i < got.size(); ++i) close = got[i] > ratios[i] - 0.01f && got[i] < ratios[i] + 0.01f;
        Check(back.layout == LayoutPreset::Custom && back.root && SplitTreeShape(*back.root) == L"VHPPP" &&
                  back.panes.size() == 3 && close,
              "restoring brings the custom tree back with its ratios");
        LayoutTabSnapshot broken = saved;
        broken.split_tree = L"VHPP";
        LayoutTab fallback;
        RestoreLayoutTab(fallback, broken, {});
        RebuildLayoutRoot(fallback);
        Check(fallback.layout == LayoutPreset::Three && fallback.root && fallback.VisiblePanes().size() == 3,
              "a shape that does not fit falls back to the preset saved with it");
        LayoutTabSnapshot preset = CaptureLayoutTab(*SplitTab(3));
        Check(preset.split_tree.empty() && LayoutTabsToJson({preset}).find(L"splitTree") == std::wstring::npos,
              "a preset tab saves no shape");
    }

    std::printf("pane_drop_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
