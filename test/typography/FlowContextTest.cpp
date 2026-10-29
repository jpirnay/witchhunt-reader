#include <gtest/gtest.h>

#include "Epub/FlowContext.h"
#include "Epub/css/CssStyle.h"

namespace {

constexpr VerticalText kAuto = VerticalText::Automatic;
constexpr VerticalText kOn = VerticalText::On;
constexpr VerticalText kOff = VerticalText::Off;

TEST(FlowContext, TheRequestIsTheBooksThenTheSettingsThenTheLanguages) {
  EXPECT_EQ(FlowContext::requestFor(kAuto, -1, true), kAuto);
  EXPECT_EQ(FlowContext::requestFor(kAuto, 1, true), kOn);
  EXPECT_EQ(FlowContext::requestFor(kOn, 2, true), kOff);
  EXPECT_EQ(FlowContext::requestFor(kOff, 0, true), kAuto);
  EXPECT_EQ(FlowContext::requestFor(kOn, -1, false), kOff);
  EXPECT_EQ(FlowContext::requestFor(kAuto, 1, false), kOff);
  EXPECT_EQ(FlowContext::requestFor(kOn, 7, true), kOn) << "an override outside 0-2 reads as none";
}

TEST(FlowContext, OnAndOffDecideOverTheBook) {
  EXPECT_FALSE(FlowContext::verticalFor(kOff, CssWritingMode::VerticalRl, PageProgression::Rtl));
  EXPECT_TRUE(FlowContext::verticalFor(kOn, CssWritingMode::HorizontalTb, PageProgression::Default));
  EXPECT_TRUE(FlowContext::verticalFor(kOn, CssWritingMode::Unset, PageProgression::Ltr));
}

TEST(FlowContext, AutomaticFollowsTheCssThenThePpd) {
  EXPECT_TRUE(FlowContext::verticalFor(kAuto, CssWritingMode::VerticalRl, PageProgression::Default));
  EXPECT_FALSE(FlowContext::verticalFor(kAuto, CssWritingMode::HorizontalTb, PageProgression::Rtl));
  EXPECT_TRUE(FlowContext::verticalFor(kAuto, CssWritingMode::Unset, PageProgression::Rtl));
  EXPECT_FALSE(FlowContext::verticalFor(kAuto, CssWritingMode::Unset, PageProgression::Default));
  EXPECT_FALSE(FlowContext::verticalFor(kAuto, CssWritingMode::Unset, PageProgression::Ltr));
  EXPECT_FALSE(FlowContext::verticalFor(kAuto, CssWritingMode::VerticalLr, PageProgression::Rtl))
      << "only vertical-rl has a turned frame";
}

TEST(FlowContext, AColophonKeepsTheBooksDirection) {
  EXPECT_TRUE(FlowContext::pageProgressionRtlFor(kAuto, false, PageProgression::Rtl));
  EXPECT_TRUE(FlowContext::pageProgressionRtlFor(kAuto, true, PageProgression::Default));
  EXPECT_TRUE(FlowContext::pageProgressionRtlFor(kOn, true, PageProgression::Ltr));
  EXPECT_FALSE(FlowContext::pageProgressionRtlFor(kAuto, false, PageProgression::Default));
  EXPECT_FALSE(FlowContext::pageProgressionRtlFor(kOff, false, PageProgression::Rtl));
  const FlowContext f = FlowContext::of(kAuto, true, PageProgression::Default);
  EXPECT_TRUE(f.vertical);
  EXPECT_TRUE(f.pageProgressionRtl);
}

}  // namespace
