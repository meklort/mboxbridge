#include "pnor_partition_table.hpp"
#include "common.h"
#include "config.h"
#include <syslog.h>
#include <endian.h>
#include <regex>
#include <fstream>
#include <algorithm>

namespace openpower
{
namespace virtual_pnor
{

namespace partition
{
namespace block
{

// The PNOR erase-block size is 4 KB
constexpr size_t size = 4096;

} // namespace block

Table::Table():
    Table(fs::path(PARTITION_FILES_LOC))
{
}

Table::Table(fs::path&& directory):
    szBlocks(0),
    imgBlocks(0),
    directory(std::move(directory)),
    numParts(0)
{
    preparePartitions();
    prepareHeader();
    hostTbl = endianFixup(tbl);
}

void Table::prepareHeader()
{
    decltype(auto) table = getNativeTable();
    table.data.magic = PARTITION_HEADER_MAGIC;
    table.data.version = PARTITION_VERSION_1;
    table.data.size = szBlocks;
    table.data.entry_size = sizeof(pnor_partition);
    table.data.entry_count = numParts;
    table.data.block_size = block::size;
    table.data.block_count = imgBlocks;
    table.checksum = details::checksum(table.data);
}

inline void Table::allocateMemory(const fs::path& tocFile)
{
    size_t num = 0;
    std::string line;
    std::ifstream file(tocFile.c_str());

    // Find number of lines in partition file - this will help
    // determine the number of partitions and hence also how much
    // memory to allocate for the partitions array.
    // The actual number of partitions may turn out to be lesser than this,
    // in case of errors.
    while (std::getline(file, line))
    {
        // Check if line starts with "partition"
        if (std::string::npos != line.find("partition", 0))
        {
            ++num;
        }
    }

    size_t totalSizeBytes = sizeof(pnor_partition_table) +
                            (num * sizeof(pnor_partition));
    size_t totalSizeAligned = align_up(totalSizeBytes, block::size);
    szBlocks = totalSizeAligned / block::size;
    imgBlocks = szBlocks;
    tbl.resize(totalSizeAligned);
}

inline void Table::writeSizes(pnor_partition& part, size_t start, size_t end)
{
    size_t size = end - start;
    part.data.base = imgBlocks;
    size_t sizeInBlocks = align_up(size, block::size) / block::size;
    imgBlocks += sizeInBlocks;
    part.data.size = sizeInBlocks;
    part.data.actual = size;
}

inline void Table::writeUserdata(pnor_partition& part, const std::string& data)
{
    if (std::string::npos != data.find("ECC"))
    {
        part.data.user.data[0] = PARTITION_ECC_PROTECTED;
    }
    auto perms = 0;
    if (std::string::npos != data.find("READONLY"))
    {
        perms |= PARTITION_READONLY;
    }
    if (std::string::npos != data.find("PRESERVED"))
    {
        perms |= PARTITION_PRESERVED;
    }
    part.data.user.data[1] = perms;
}

inline void Table::writeDefaults(pnor_partition& part)
{
    part.data.pid = PARENT_PATITION_ID;
    part.data.type = PARTITION_TYPE_DATA;
    part.data.flags = 0; // flags unused
}

inline void Table::writeNameAndId(pnor_partition& part, std::string&& name,
                                  const std::string& id)
{
    name.resize(PARTITION_NAME_MAX);
    memcpy(part.data.name,
           name.c_str(),
           sizeof(part.data.name));
    part.data.id = std::stoul(id);
}

void Table::preparePartitions()
{
    fs::path tocFile = directory;
    tocFile /= PARTITION_TOC_FILE;
    allocateMemory(tocFile);

    std::ifstream file(tocFile.c_str());
    static constexpr auto ID_MATCH = 1;
    static constexpr auto NAME_MATCH = 2;
    static constexpr auto START_ADDR_MATCH = 3;
    static constexpr auto END_ADDR_MATCH = 4;
    // Parse PNOR toc (table of contents) file, which has lines like :
    // partition01=HBB,00010000,000a0000,ECC,PRESERVED, to indicate partitions
    std::regex regex
    {
        "^partition([0-9]+)=([A-Za-z0-9_]+),"
        "([0-9a-fA-F]+),([0-9a-fA-F]+)",
        std::regex::extended
    };
    std::smatch match;
    std::string line;

    decltype(auto) table = getNativeTable();

    while (std::getline(file, line))
    {
        if (std::regex_search(line, match, regex))
        {
            fs::path partitionFile = directory;
            partitionFile /= match[NAME_MATCH].str();
            if (!fs::exists(partitionFile))
            {
                MSG_ERR("Partition file %s does not exist",
                         partitionFile.c_str());
                continue;
            }

            writeNameAndId(table.partitions[numParts],
                           match[NAME_MATCH].str(),
                           match[ID_MATCH].str());
            writeDefaults(table.partitions[numParts]);
            writeSizes(table.partitions[numParts],
                       std::stoul(match[START_ADDR_MATCH].str(), nullptr, 16),
                       std::stoul(match[END_ADDR_MATCH].str(), nullptr, 16));
            writeUserdata(table.partitions[numParts], match.suffix().str());
            table.partitions[numParts].checksum =
                details::checksum(table.partitions[numParts].data);

            ++numParts;
        }
    }
}

const pnor_partition& Table::partition(size_t offset) const
{
    const decltype(auto) table = getNativeTable();
    size_t offt = offset / block::size;

    for (decltype(numParts) i{}; i < numParts; ++i)
    {
        if ((offt >= table.partitions[i].data.base) &&
            (offt < (table.partitions[i].data.base +
                     table.partitions[i].data.size)))
        {
            return table.partitions[i];
        }
    }

    static pnor_partition p{};
    return p;
}

} // namespace partition

PartitionTable endianFixup(const PartitionTable& in)
{
    PartitionTable out;
    out.resize(in.size());
    auto src = reinterpret_cast<const pnor_partition_table*>(in.data());
    auto dst = reinterpret_cast<pnor_partition_table*>(out.data());

    dst->data.magic = htobe32(src->data.magic);
    dst->data.version = htobe32(src->data.version);
    dst->data.size = htobe32(src->data.size);
    dst->data.entry_size = htobe32(src->data.entry_size);
    dst->data.entry_count = htobe32(src->data.entry_count);
    dst->data.block_size = htobe32(src->data.block_size);
    dst->data.block_count = htobe32(src->data.block_count);
    dst->checksum = htobe32(src->checksum);

    for (decltype(src->data.entry_count) i{}; i < src->data.entry_count; ++i)
    {
        auto psrc = &src->partitions[i];
        auto pdst = &dst->partitions[i];
        strncpy(pdst->data.name, psrc->data.name, PARTITION_NAME_MAX);
        // Just to be safe
        pdst->data.name[PARTITION_NAME_MAX] = '\0';
        pdst->data.base = htobe32(psrc->data.base);
        pdst->data.size = htobe32(psrc->data.size);
        pdst->data.pid = htobe32(psrc->data.pid);
        pdst->data.id = htobe32(psrc->data.id);
        pdst->data.type = htobe32(psrc->data.type);
        pdst->data.flags = htobe32(psrc->data.flags);
        pdst->data.actual = htobe32(psrc->data.actual);
        for (size_t j = 0; j < PARTITION_USER_WORDS; ++j)
        {
            pdst->data.user.data[j] = htobe32(psrc->data.user.data[j]);
        }
        pdst->checksum = htobe32(psrc->checksum);
    }

    return out;
}

} // namespace virtual_pnor
} // namespace openpower