#include <range/v3/algorithm.hpp>

#include <cpu/cyclic32.h>
#include <mio/dma_ctrl.h>
#include <mio/mem_pipe.h>
#include <engine/ee_core.h>
#include <vu/vif10_upload.h>
#include <vu/vecu.h>
namespace cosmic::mio {
    void DmaController::connectDevices(HardWithDmaCap& devices) {
        hw.vif0 = devices.vif0;
        hw.vif1 = devices.vif1;
        hw.core = devices.ee;
    }
    DmaController::DmaController() {
        queued.resize(channels.size());
        getStagedChannel = [&](const DmaChannelId requested) -> ChannelIterator {
            ChannelIterator b, e;
            b = std::begin(queued);
            e = std::end(queued);
            std::array<u32, 2> crc;

            for (; b != e; b++) {
                ChannelIterator::value_type
                    local{channels[requested]},
                        staged{*b};
                crc[0] = cpu::check32({BitCast<u8*>(&staged), sizeof(staged)});
                crc[1] = cpu::check32({BitCast<u8*>(&local), sizeof(local)});
                if (crc[0] == crc[1]) {
                    e = b;
                    break;
                }
            }
            return e;
        };

        std::list<DmaChannel> emptyDma{};
        queued.swap(emptyDma);
    }

    void DmaController::resetMa() {
        status.isDmaEnabled = false;
        // According to DobieStation, it is a requirement for the SCPH-39001 BIOS
        ir.dicr = 0x1201;

        for (u8 dmIn{}; dmIn < 9; dmIn++) {
            channels[dmIn].index = dmIn;
            switch (dmIn) {
            // These channels do not have a specified FIFO; thus, they can be executed as soon as they arrive
            case Vif0:
            case Vif1:
            case IpuTo:
            case Sif1:
            case SprFrom:
            case SprTo:
                channels[dmIn].request = true;
            }
            channels[dmIn].request = false;
        }
        priorityCtrl = 0;
        // stallAddress = 0;
        highCycles = 0;
        queued.clear();
    }
    void DmaController::pulse(u32 cycles) {
        bool masterEnb{status.isDmaEnabled};
        if (!masterEnb || ir.busError)
            return;

        if (hasOwner)
            highCycles += cycles;
        u32 reduce{};

        for (; hasOwner && highCycles > 0; ) {
            Ref<DmaChannel> tv{};
            tv = std::ref(channels.at(hasOwner.id));
            switch (tv->index) {
            case Vif0:
                reduce = feedVif0Pipe(tv).first; break;
            }

            highCycles -= std::max(reduce, static_cast<u32>(1));
            if (tv->isScratch)
                highCycles -= 0xc;

            if (!hasOwner)
                highCycles = 0;
        }
        highCycles = 0;
    }
    Ref<u32> DmaController::dmaVirtSolver(u32 address) {
        u64 invCid = channels.size();
        u64 cid = invCid;
        u8 which{};

        switch (address >> 12) {
        case 0x8:
        case 0x9:
        case 0xa ... 0xd:
            cid = (address >> 12) - 0x8; break;
        }
        if ((address >> 16 & 0x1000) != 0x1000) {
            throw MioErr("(DMA): Reading from an invalid address, unreachable address {}", address);
        } else if (cid == invCid) {
            throw MioErr("No channel selected, very serious error...");
        }
        // For specific channels like: SifX, IpuX, SprX
        if ((address >> 4 & 0x400) == 0x400)
            cid++;

        which = address & 0xff;
        if ((address >> 12 & 0xe000) != 0xe000) {
            if (which == 0x10)
                return channels[cid].adr;
            else if (which == 0x20)
                return *BitCast<u32*>(&channels[cid].qwc);
        } else {
            switch (which) {
            case 0x10:
                return intStatus;
            case 0x20:
                return priorityCtrl.value;
            }
        }
        return {};
    }
    std::pair<u32, u8> DmaController::feedVif0Pipe(Ref<DmaChannel> vifc) {
        u32 transferred{};
        auto [haveData, count] = pipeQuad2Transfer(vifc);
        if (!haveData) {
            u32 remainFifoSpace{hw.vif0->getFifoFreeSpace()};
            switch (remainFifoSpace) {
            case 8:
                hw.vif0->transferDmaData(dmacRead(vifc->adr));
                advanceSrcDma(vifc);

                hw.vif0->transferDmaData(dmacRead(vifc->adr));
                advanceSrcDma(vifc);
                hw.vif0->transferDmaData(dmacRead(vifc->adr));
                advanceSrcDma(vifc);
                hw.vif0->transferDmaData(dmacRead(vifc->adr));
                advanceSrcDma(vifc);
                transferred += 4;
            case 4:
                hw.vif0->transferDmaData(dmacRead(vifc->adr));
                advanceSrcDma(vifc);
                hw.vif0->transferDmaData(dmacRead(vifc->adr));
                advanceSrcDma(vifc);
                transferred += 2;
            case 2:
                hw.vif0->transferDmaData(dmacRead(vifc->adr));
                advanceSrcDma(vifc);
                hw.vif0->transferDmaData(dmacRead(vifc->adr));
                advanceSrcDma(vifc);
                transferred += 2;
            }

            while (remainFifoSpace-- > 0 &&
                transferred < count) {
                hw.vif0->transferDmaData(dmacRead(vifc->adr), true);
                advanceSrcDma(vifc);
                transferred++;
            }
        }
        if (!vifc->qwc) {
        } else {
            switchChannel();
        }
        return {transferred, 0};
    }

    std::pair<bool, u32> DmaController::pipeQuad2Transfer(Ref<DmaChannel> ch) {
        constexpr u8 qwcPerRequest{8};
        if (!ch->qwc)
            return std::make_pair(false, 0);
        u32 maxQwc{qwcPerRequest - (ch->adr >> 0x4) & 0x7};
        if (maxQwc >= std::numeric_limits<u16>::max()) {
        }

        const u32 toTransfer{std::min(ch->qwc, static_cast<u16>(maxQwc))};
        return {true, toTransfer};
    }
    void DmaController::disableChannel(DirectChannels channel, bool disableRequest) {
        bool isDisable{!disableRequest};
        u32 index{static_cast<u32>(channel)};
        auto& tv{channels.at(index)};

        if (disableRequest) {
            isDisable = tv.request;
            tv.index = false;
            findNextChannel();
        }
        if (!isDisable)
            return;

        if (hasOwner.id == index) {
            hasOwner.unselect();
            return;
        }
        auto del{getStagedChannel(static_cast<DmaChannelId>(index))};
        // Only one stream to remove? I don't understand why yet
        if (del != std::end(queued)) {
            queued.erase(del);
        }
    }
    os::vec DmaController::dmacRead(u32& address) {
        bool isScratchPad{address & (static_cast<u32>(1 << 31)) ||
            (address & 0x70000000) == 0x70000000};
        bool isVuArea{address >= 0x11000000 && address < 0x11010000};
        auto vd{*dmaAddrSolver(address, isScratchPad, isVuArea)};

        return vd;
    }
    void DmaController::dmacWrite(u32 address, const os::vec& val) {
        std::array<bool, 2> is;
        is[0] = address & (static_cast<u32>(1 << 31)) || (address & 0x70000000) == 0x70000000;
        is[1] = address >= 0x11000000 && address < 0x11010000;

        *dmaAddrSolver(address, is[0], is[1]) = val.get();
    }

    Ref<u128> DmaController::dmaAddrSolver(u32 address, bool isScr, bool isVu) {
        if (isScr) {
            return *BitCast<u128*>(&hw.core->scratchPad[address & 0x3ff0]);
        } else if (!isVu) {
            return *PipeCraftPtr<u128*>(pipe, address & 0x01fffff0);
        }
        Ref<vu::VuWorkMemory> vu01Mem{};
        u32 mask;
        if (address < 0x11008000) {
            vu01Mem = std::ref(hw.vif0->vifVu->vecRegion);
            mask = hw.vif0->vifVu->getMemMask();
        } else {
            vu01Mem = std::ref(hw.vif1->vifVu->vecRegion);
            mask = hw.vif1->vifVu->getMemMask();
        }
        bool is0Inst{address < 0x11004000};
        bool is0Data{address < 0x11008000};
        bool is1Inst{address < 0x1100C000};

        if (is0Inst) {
            // Reading from VU0::TEXT
            return *BitCast<u128*>(&hw.vif0->vifVu->vecRegion.re[address & mask]);
        } else if (is0Data || !is1Inst) {
            // Reading from VU0::DATA or VU1::DATA
            return *BitCast<u128*>(&vu01Mem->rw[address & mask]);
        }
        // Reading from VU1::TEXT
        return *BitCast<u128*>(&hw.vif1->vifVu->vecRegion.re[address & mask]);
    }
}
