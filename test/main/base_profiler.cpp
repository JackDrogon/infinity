//
// Created by JinHai on 2022/9/6.
//

#include <gtest/gtest.h>
#include "base_test.h"
#include "main/base_profiler.h"

class BaseProfilerTest : public BaseTest {

};

TEST_F(BaseProfilerTest, test1) {
    infinity::BaseProfiler profiler;
    profiler.Begin();
    profiler.End();
//    std::cout << profiler.Elapsed() << std::endl;
    EXPECT_LT(profiler.Elapsed(), 1000);
    EXPECT_NE(profiler.ElapsedToString().find("ns"), std::string::npos);

    // Test the time duration larger than 1us
    profiler.Begin();
    usleep(1);
    profiler.End();
    EXPECT_GT(profiler.Elapsed(), 1000);
    EXPECT_LT(profiler.Elapsed(), 1000*1000);
    EXPECT_NE(profiler.ElapsedToString().find("us"), std::string::npos);

    // Test the time duration larger than 1ms
    profiler.Begin();
    usleep(1000);
    profiler.End();
    EXPECT_GT(profiler.Elapsed(), 1000*1000);
    EXPECT_LT(profiler.Elapsed(), 1000*1000*1000);
    EXPECT_NE(profiler.ElapsedToString().find("ms"), std::string::npos);

    // Test the time duration larger than 1s
    profiler.Begin();
    usleep(1000*1000);
    profiler.End();
    EXPECT_GT(profiler.Elapsed(), 1000*1000*1000);
    EXPECT_LT(profiler.Elapsed(), 1000*1000*1000*1000L);
    EXPECT_NE(profiler.ElapsedToString().find('s'), std::string::npos);

//    std::cout << profiler.Elapsed() << std::endl;
//    std::cout << profiler.ElapsedToString() << std::endl;
}