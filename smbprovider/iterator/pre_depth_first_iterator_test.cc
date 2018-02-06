// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "smbprovider/fake_samba_interface.h"
#include "smbprovider/iterator/pre_depth_first_iterator.h"
#include "smbprovider/smbprovider_helper.h"
#include "smbprovider/smbprovider_test_helper.h"

namespace smbprovider {

class PreDepthFirstIteratorTest : public testing::Test {
 public:
  PreDepthFirstIteratorTest() {}
  ~PreDepthFirstIteratorTest() override = default;

 protected:
  void CreateDefaultMountRoot() {
    fake_samba_.AddDirectory(GetDefaultServer());
    fake_samba_.AddDirectory(GetDefaultMountRoot());
  }

  FakeSambaInterface fake_samba_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PreDepthFirstIteratorTest);
};

// PreDepthFirstIterator iterates over a single empty directory correctly.
TEST_F(PreDepthFirstIteratorTest, ItSucceedsOnEmptyDir) {
  CreateDefaultMountRoot();

  fake_samba_.AddDirectory(GetDefaultFullPath("/path"));

  PreDepthFirstIterator it(GetDefaultFullPath("/path"), &fake_samba_);

  EXPECT_EQ(0, it.Init());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("path", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_TRUE(it.IsDone());
}

// PreDepthFirstIterator iteratres over multiple nested dirs correctly.
TEST_F(PreDepthFirstIteratorTest, ItSuceedsOnNestedEmptyDirs) {
  CreateDefaultMountRoot();

  fake_samba_.AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_.AddDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_.AddDirectory(GetDefaultFullPath("/path/dogs/cats"));

  PreDepthFirstIterator it(GetDefaultFullPath("/path"), &fake_samba_);

  EXPECT_EQ(0, it.Init());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("path", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("dogs", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("cats", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_TRUE(it.IsDone());
}

// PreDepthFirstIterator iterates over a single dir of files correctly.
TEST_F(PreDepthFirstIteratorTest, ItSucceedsOnSingleDirOfFiles) {
  CreateDefaultMountRoot();

  fake_samba_.AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/1.jpg"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/2.txt"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/3.png"));

  PreDepthFirstIterator it(GetDefaultFullPath("/path"), &fake_samba_);

  EXPECT_EQ(0, it.Init());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("path", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("1.jpg", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("2.txt", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("3.png", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_TRUE(it.IsDone());
}

// PreDepthFirstIterator iterates over a dir with a file and a dir correctly.
TEST_F(PreDepthFirstIteratorTest, ItSucceedsOnDirWithFileAndNonEmptyDir) {
  CreateDefaultMountRoot();

  fake_samba_.AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_.AddDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/dogs/1.jpg"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/2.txt"));

  PreDepthFirstIterator it(GetDefaultFullPath("/path"), &fake_samba_);

  EXPECT_EQ(0, it.Init());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("path", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("dogs", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("1.jpg", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("2.txt", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_TRUE(it.IsDone());
}

// PreDepthFirstIterator correctly iterates over a complex filesystem.
//                            path/
//                         /   /    \
//                       1  dogs/     cats/
//                         /  |   \    /  \
//                        2 mouse/ 3   4   5
TEST_F(PreDepthFirstIteratorTest, NextSucceedsOnComplexMultiLevelFileSystem) {
  CreateDefaultMountRoot();

  fake_samba_.AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/1.jpg"));
  fake_samba_.AddDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/dogs/2.jpg"));
  fake_samba_.AddDirectory(GetDefaultFullPath("/path/dogs/mouse"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/dogs/3.jpg"));
  fake_samba_.AddDirectory(GetDefaultFullPath("/path/cats"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/cats/4.jpg"));
  fake_samba_.AddFile(GetDefaultFullPath("/path/cats/5.jpg"));

  PreDepthFirstIterator it(GetDefaultFullPath("/path"), &fake_samba_);

  EXPECT_EQ(0, it.Init());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("path", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("1.jpg", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("dogs", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("2.jpg", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("mouse", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("3.jpg", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("cats", it.Get().name);
  EXPECT_TRUE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("4.jpg", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_FALSE(it.IsDone());
  EXPECT_EQ("5.jpg", it.Get().name);
  EXPECT_FALSE(it.Get().is_directory);
  EXPECT_EQ(0, it.Next());

  EXPECT_TRUE(it.IsDone());
}

}  // namespace smbprovider
