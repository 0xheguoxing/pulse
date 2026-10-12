// pane_drop_test.cpp - Multi-window M7h: a tab split in panes dropped on an
// edge of another window's content brings all its panes, up to four in all
// (the prototype's addSplit), in their order, with a preview per pane.
#include "../app/pane_drop.h"

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

    std::printf("pane_drop_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
