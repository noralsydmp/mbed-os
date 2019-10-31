#include "gtest/gtest.h"
#include "features/storage/blockdevice/HeapBlockDevice.h"
#include "string.h"
#include "mbed_assert.h"

#define BLOCK_SIZE (512)
#define DEVICE_SIZE (BLOCK_SIZE*10)

class HeapBlockDeviceTest : public testing::Test {
protected:
    virtual void SetUp()
    {
        bd.init();
    }

    virtual void TearDown()
    {
        bd.deinit();
    }

    mbed::HeapBlockDevice bd{DEVICE_SIZE};
};

TEST_F(HeapBlockDeviceTest, constructor)
{
    // HeapBlockDevice(bd_size_t size, bd_size_t read, bd_size_t program, bd_size_t erase);
    mbed::HeapBlockDevice one{3000, 100, 200, 300};
    EXPECT_EQ(one.init(), BD_ERROR_OK);
    EXPECT_EQ(one.size(), 3000);
    EXPECT_EQ(one.get_read_size(), 100);
    EXPECT_EQ(one.get_program_size(), 200);
    EXPECT_EQ(one.get_erase_size(), 300);
    EXPECT_EQ(one.get_erase_size(0), 300);
    EXPECT_EQ(one.deinit(), BD_ERROR_OK);
}

TEST_F(HeapBlockDeviceTest, double_init)
{
    mbed::HeapBlockDevice one{DEVICE_SIZE};
    EXPECT_EQ(one.init(), BD_ERROR_OK);
    EXPECT_EQ(one.init(), BD_ERROR_OK);
    EXPECT_EQ(one.deinit(), BD_ERROR_OK); // First de-init does only decrement the counter
    EXPECT_EQ(one.deinit(), BD_ERROR_OK);
    EXPECT_EQ(one.deinit(), BD_ERROR_OK); //Third one does not de-init, but return immediately
}

TEST_F(HeapBlockDeviceTest, get_type)
{
    EXPECT_EQ(0, strcmp(bd.get_type(), "HEAP"));
}

TEST_F(HeapBlockDeviceTest, erase_program_read)
{
    uint8_t *block = new uint8_t[BLOCK_SIZE]{0xaa,0xbb,0xcc};
    uint8_t *buf = new uint8_t[BLOCK_SIZE];
    EXPECT_EQ(bd.erase(0, BLOCK_SIZE),          BD_ERROR_OK);
    EXPECT_EQ(bd.program(block, 0, BLOCK_SIZE), BD_ERROR_OK);
    EXPECT_EQ(bd.read(buf, 0, BLOCK_SIZE),      BD_ERROR_OK);
    EXPECT_EQ(0, memcmp(block, buf, BLOCK_SIZE));
    delete[] block;
    delete[] buf;
}

TEST_F(HeapBlockDeviceTest, use_uninitalized)
{
    mbed::HeapBlockDevice one{DEVICE_SIZE};
    uint8_t *buf = new uint8_t[BLOCK_SIZE];
    EXPECT_EQ(one.read(buf, 0, BLOCK_SIZE), BD_ERROR_DEVICE_ERROR);
    EXPECT_EQ(one.program(buf, 0, BLOCK_SIZE), BD_ERROR_DEVICE_ERROR);
    delete[] buf;
}

TEST_F(HeapBlockDeviceTest, over_read)
{
    uint8_t *buf = new uint8_t[BLOCK_SIZE];
    EXPECT_EQ(bd.read(buf, DEVICE_SIZE, BLOCK_SIZE), BD_ERROR_DEVICE_ERROR);
    delete[] buf;
}

TEST_F(HeapBlockDeviceTest, over_write)
{
    uint8_t *buf = new uint8_t[BLOCK_SIZE]{0xaa,0xbb,0xcc};
    EXPECT_EQ(bd.program(buf, DEVICE_SIZE, BLOCK_SIZE), BD_ERROR_DEVICE_ERROR);
    delete[] buf;
}

TEST_F(HeapBlockDeviceTest, over_erase)
{
    EXPECT_EQ(bd.erase(DEVICE_SIZE, BLOCK_SIZE), BD_ERROR_DEVICE_ERROR);
}

TEST_F(HeapBlockDeviceTest, erase_uninitialized)
{
    mbed::HeapBlockDevice one{DEVICE_SIZE};
    EXPECT_EQ(one.erase(DEVICE_SIZE, BLOCK_SIZE), BD_ERROR_DEVICE_ERROR);
}

TEST_F(HeapBlockDeviceTest, read_unprogrammed)
{
    uint8_t *buf = new uint8_t[BLOCK_SIZE];
    EXPECT_EQ(bd.read(buf, DEVICE_SIZE - BLOCK_SIZE, BLOCK_SIZE), BD_ERROR_OK);
    // Ignore the content, it is now zero, but does not need to be.
    delete[] buf;
}
