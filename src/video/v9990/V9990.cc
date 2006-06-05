// $Id$

#include "V9990.hh"
#include "Scheduler.hh"
#include "Display.hh"
#include "RendererFactory.hh"
#include "V9990VRAM.hh"
#include "V9990CmdEngine.hh"
#include "V9990Renderer.hh"
#include "SimpleDebuggable.hh"
#include "MSXMotherBoard.hh"
#include <cassert>

namespace openmsx {

class V9990RegDebug : public SimpleDebuggable
{
public:
	V9990RegDebug(V9990& v9990);
	virtual byte read(unsigned address);
	virtual void write(unsigned address, byte value, const EmuTime& time);
private:
	V9990& v9990;
};

class V9990PalDebug : public SimpleDebuggable
{
public:
	V9990PalDebug(V9990& v9990);
	virtual byte read(unsigned address);
	virtual void write(unsigned address, byte value, const EmuTime& time);
private:
	V9990& v9990;
};


enum RegisterAccess { NO_ACCESS, RD_ONLY, WR_ONLY, RD_WR };
static const RegisterAccess regAccess[64] = {
	WR_ONLY, WR_ONLY, WR_ONLY,          // VRAM Write Address
	WR_ONLY, WR_ONLY, WR_ONLY,          // VRAM Read Address
	RD_WR, RD_WR,                       // Screen Mode
	RD_WR,                              // Control
	RD_WR, RD_WR, RD_WR, RD_WR,         // Interrupt
	WR_ONLY,                            // Palette Control
	WR_ONLY,                            // Palette Pointer
	RD_WR,                              // Back Drop Color
	RD_WR,                              // Display Adjust
	RD_WR, RD_WR, RD_WR, RD_WR,         // Scroll Control A
	RD_WR, RD_WR, RD_WR, RD_WR,         // Scroll Control B
	RD_WR,                              // Sprite Pattern Table Adress
	RD_WR,                              // LCD Control
	RD_WR,                              // Priority Control
	WR_ONLY,                            // Sprite Palette Control
	NO_ACCESS, NO_ACCESS, NO_ACCESS,    // 3x not used
	WR_ONLY, WR_ONLY, WR_ONLY, WR_ONLY, // Cmd Parameter Src XY
	WR_ONLY, WR_ONLY, WR_ONLY, WR_ONLY, // Cmd Parameter Dest XY
	WR_ONLY, WR_ONLY, WR_ONLY, WR_ONLY, // Cmd Parameter Size XY
	WR_ONLY, WR_ONLY, WR_ONLY, WR_ONLY, // Cmd Parameter Arg, LogOp, WrtMask
	WR_ONLY, WR_ONLY, WR_ONLY, WR_ONLY, // Cmd Parameter Font Color
	WR_ONLY, RD_ONLY, RD_ONLY,          // Cmd Parameter OpCode, Border X
	NO_ACCESS, NO_ACCESS, NO_ACCESS,    // registers 55-63
	NO_ACCESS, NO_ACCESS, NO_ACCESS,
	NO_ACCESS, NO_ACCESS, NO_ACCESS
};

// -------------------------------------------------------------------------
// Constructor & Destructor
// -------------------------------------------------------------------------

V9990::V9990(MSXMotherBoard& motherBoard, const XMLElement& config,
             const EmuTime& time)
	: MSXDevice(motherBoard, config, time)
	, Schedulable(motherBoard.getScheduler())
	, irq(motherBoard.getCPU())
	, pendingIRQs(0)
	, frameStartTime(time)
	, hScanSyncTime(time)
	, v9990RegDebug(new V9990RegDebug(*this))
	, v9990PalDebug(new V9990PalDebug(*this))
{
        // clear regs TODO find realistic init values
        memset(regs, 0, sizeof(regs));
	calcDisplayMode();

	// initialize palette
	for (int i = 0; i < 64; ++i) {
		palette[4 * i + 0] = 0x9F;
		palette[4 * i + 1] = 0x1F;
		palette[4 * i + 2] = 0x1F;
		palette[4 * i + 3] = 0x00;
	}

	// create VRAM
	vram.reset(new V9990VRAM(*this, time));

	// create Command Engine
	cmdEngine.reset(new V9990CmdEngine(*this, time,
		getMotherBoard().getDisplay().getRenderSettings()));

	// Start with NTSC timing
	palTiming = false;
	setVerticalTiming();

	// Initialise rendering system
	createRenderer(time);

	reset(time);
	getMotherBoard().getDisplay().attach(*this);
}

V9990::~V9990()
{
	getMotherBoard().getDisplay().detach(*this);
}

// -------------------------------------------------------------------------
// MSXDevice
// -------------------------------------------------------------------------

void V9990::reset(const EmuTime& time)
{
	removeSyncPoint(V9990_VSYNC);
	removeSyncPoint(V9990_DISPLAY_START);
	removeSyncPoint(V9990_VSCAN);
	removeSyncPoint(V9990_HSCAN);
	removeSyncPoint(V9990_SET_MODE);
	removeSyncPoint(V9990_SET_BLANK);

	// Clear registers / ports
	memset(regs, 0, sizeof(regs));
	status = 0;
	regSelect = 0xFF; // TODO check value for power-on and reset
	calcDisplayMode();

	isDisplayArea = false;
	displayEnabled = false;

	// Reset IRQs
	writeIO(INTERRUPT_FLAG, 0xFF, time);

	palTiming = false;
	// Reset sub-systems
	renderer->reset(time);
	cmdEngine->reset(time);

	// Init scheduling
	frameStart(time);
}

byte V9990::readIO(word port, const EmuTime& time)
{
	port &= 0x0F;

	byte result = 0;
	switch (port) {
		case VRAM_DATA: {
			// read from VRAM
			unsigned addr = getVRAMAddr(VRAM_READ_ADDRESS_0);
			result = vram->readVRAMSlow(addr);
			if (!(regs[VRAM_READ_ADDRESS_2] & 0x80)) {
				setVRAMAddr(VRAM_READ_ADDRESS_0, addr + 1);
			}
			break;
		}
		case PALETTE_DATA: {
			byte& palPtr = regs[PALETTE_POINTER];
			result = palette[palPtr];
			if (!(regs[PALETTE_CONTROL] & 0x10)) {
				switch (palPtr & 3) {
				case 0:  palPtr += 1; break; // red
				case 1:  palPtr += 1; break; // green
				case 2:  palPtr += 2; break; // blue
				default: palPtr -= 3; break; // checked on real V9990
				}
			}
			break;
		}
		case COMMAND_DATA:
			// TODO
			//assert(cmdEngine != NULL);
			result = cmdEngine->getCmdData(time);
			break;

		case REGISTER_DATA: {
			// read register
			result = readRegister(regSelect & 0x3F, time);
			if (!(regSelect & 0x40)) {
				regSelect =   regSelect      & 0xC0 |
				            ((regSelect + 1) & 0x3F);
			}
			break;
		}
		case INTERRUPT_FLAG:
			result = pendingIRQs;
			break;

		case STATUS: {
			int ticks = getUCTicksThisFrame(time);
			int x = UCtoX(ticks, getDisplayMode());
			int y = ticks / V9990DisplayTiming::UC_TICKS_PER_LINE;
			bool hr = (x < 64) || (576 <= x); // TODO not correct
			bool vr = (y < 14) || (226 <= y); // TODO not correct
			result = cmdEngine->getStatus(time) |
				 (vr ? 0x40 : 0x00) |
				 (hr ? 0x20 : 0x00) |
				 (status & 0x06);
			break;
		}
		case KANJI_ROM_1:
		case KANJI_ROM_3:
			// not used in Gfx9000
			result = 0xFF;	// TODO check
			break;

		case REGISTER_SELECT:
		case SYSTEM_CONTROL:
		case KANJI_ROM_0:
		case KANJI_ROM_2:
		default:
			// write-only
			result = 0xFF;
			break;
	}

	//PRT_DEBUG("[" << time << "] "
	//	  "V9990::readIO - port=0x" << std::hex << (int)port <<
	//	                  " val=0x" << std::hex << (int)result);

	return result;
}

byte V9990::peekIO(word /*port*/, const EmuTime& /*time*/) const
{
	// TODO not implemented
	return 0xFF;
}

void V9990::writeIO(word port, byte val, const EmuTime& time)
{
	port &= 0x0F;

	//PRT_DEBUG("[" << time << "] "
	//	  "V9990::writeIO - port=0x" << std::hex << int(port) <<
	//	                   " val=0x" << std::hex << int(val));

	switch (port) {
		case VRAM_DATA: {
			// write VRAM
			unsigned addr = getVRAMAddr(VRAM_WRITE_ADDRESS_0);
			vram->writeVRAMSlow(addr, val);
			if (!(regs[VRAM_WRITE_ADDRESS_2] & 0x80)) {
				setVRAMAddr(VRAM_WRITE_ADDRESS_0, addr + 1);
			}
			break;
		}
		case PALETTE_DATA: {
			byte& palPtr = regs[PALETTE_POINTER];
			writePaletteRegister(palPtr, val, time);
			switch (palPtr & 3) {
				case 0:  palPtr += 1; break; // red
				case 1:  palPtr += 1; break; // green
				case 2:  palPtr += 2; break; // blue
				default: palPtr -= 3; break; // checked on real V9990
			}
			break;
		}
		case COMMAND_DATA:
			//assert(cmdEngine != NULL);
			cmdEngine->setCmdData(val, time);
			break;

		case REGISTER_DATA: {
			// write register
			writeRegister(regSelect & 0x3F, val, time);
			if (!(regSelect & 0x80)) {
				regSelect =   regSelect      & 0xC0 |
				            ((regSelect + 1) & 0x3F);
			}
			break;
		}
		case REGISTER_SELECT:
			regSelect = val;
			break;

		case STATUS:
			// read-only, ignore writes
			break;

		case INTERRUPT_FLAG:
			pendingIRQs &= ~val;
			if (!(pendingIRQs & regs[INTERRUPT_0])) {
				irq.reset();
			}
			scheduleHscan(time);
			break;

		case SYSTEM_CONTROL:
			status = (status & 0xFB) | ((val & 1) << 2);
			syncAtNextLine(V9990_SET_MODE, time);
		break;

		case KANJI_ROM_0:
		case KANJI_ROM_1:
		case KANJI_ROM_2:
		case KANJI_ROM_3:
			// not used in Gfx9000, ignore
			break;

		default:
			// ignore
			break;
	}
}

// =========================================================================
// Private stuff
// =========================================================================

// -------------------------------------------------------------------------
// Schedulable
// -------------------------------------------------------------------------

void V9990::executeUntil(const EmuTime& time, int userData)
{
	//PRT_DEBUG("[" << time << "] "
	//          "V9990::executeUntil - data=0x" << std::hex << userData);
	switch (userData)  {
	case V9990_VSYNC:
		// Transition from one frame to the next
		renderer->frameEnd(time);
		frameStart(time);
		break;

	case V9990_DISPLAY_START:
		if (displayEnabled) {
			renderer->updateDisplayEnabled(true, time);
		}
		isDisplayArea = true;
		break;

	case V9990_VSCAN:
		if (isDisplayEnabled()) {
			renderer->updateDisplayEnabled(false, time);
		}
		isDisplayArea = false;
		raiseIRQ(VER_IRQ);
		break;

	case V9990_HSCAN:
		raiseIRQ(HOR_IRQ);
		break;

	case V9990_SET_MODE:
		calcDisplayMode();
		renderer->setDisplayMode(getDisplayMode(), time);
		renderer->setColorMode(getColorMode(), time);
		break;

	case V9990_SET_BLANK: {
		bool newDisplayEnabled = regs[CONTROL] & 0x80;
		if (isDisplayArea) {
			renderer->updateDisplayEnabled(newDisplayEnabled, time);
		}
		displayEnabled = newDisplayEnabled;
		break;
	}
	default:
		assert(false);
	}
}

const std::string& V9990::schedName() const
{
	static const std::string name("V9990");
	return name;
}

// -------------------------------------------------------------------------
// VideoSystemChangeListener
// -------------------------------------------------------------------------

void V9990::preVideoSystemChange()
{
	renderer.reset();
}

void V9990::postVideoSystemChange()
{
	const EmuTime& time = getMotherBoard().getScheduler().getCurrentTime();
	createRenderer(time);
	renderer->frameStart(time);
}

// -------------------------------------------------------------------------
// V9990RegDebug
// -------------------------------------------------------------------------

V9990RegDebug::V9990RegDebug(V9990& v9990_)
	: SimpleDebuggable(v9990_.getMotherBoard(),
	                   v9990_.getName() + " regs", "V9990 registers", 0x40)
	, v9990(v9990_)
{
}

byte V9990RegDebug::read(unsigned address)
{
	return v9990.regs[address];
}

void V9990RegDebug::write(unsigned address, byte value, const EmuTime& time)
{
	v9990.writeRegister(address, value, time);
}

// -------------------------------------------------------------------------
// V9990PalDebug
// -------------------------------------------------------------------------

V9990PalDebug::V9990PalDebug(V9990& v9990_)
	: SimpleDebuggable(v9990_.getMotherBoard(),
	                   v9990_.getName() + " palette",
	                   "V9990 palette (format is R, G, B, 0).", 0x100)
	, v9990(v9990_)
{
}

byte V9990PalDebug::read(unsigned address)
{
	return v9990.palette[address];
}

void V9990PalDebug::write(unsigned address, byte value, const EmuTime& time)
{
	v9990.writePaletteRegister(address, value, time);
}

// -------------------------------------------------------------------------
// Private methods
// -------------------------------------------------------------------------

inline unsigned V9990::getVRAMAddr(RegisterId base) const
{
	return   regs[base + 0] +
	        (regs[base + 1] << 8) +
	       ((regs[base + 2] & 0x07) << 16);
}

inline void V9990::setVRAMAddr(RegisterId base, unsigned addr)
{
	regs[base + 0] =   addr &     0xFF;
	regs[base + 1] =  (addr &   0xFF00) >> 8;
	regs[base + 2] = ((addr & 0x070000) >> 16) | regs[base + 2] & 0x80;
	// TODO check
}

byte V9990::readRegister(byte reg, const EmuTime& time)
{
	// TODO sync(time) (if needed at all)

	assert(reg < 64);
	byte result;
	if (regAccess[reg] != NO_ACCESS && regAccess[reg] != WR_ONLY) {
		if (reg < CMD_PARAM_BORDER_X_0) {
			result = regs[reg];
		} else {
			word borderX = cmdEngine->getBorderX(time);
			return (reg == CMD_PARAM_BORDER_X_0)
			       ? (borderX & 0xFF) : (borderX >> 8);
		}
	} else {
		result = 0xFF;
	}

	//PRT_DEBUG("[" << time << "] "
	//	  "V9990::readRegister - reg=0x" << std::hex << (int)reg <<
	//	                       " val=0x" << std::hex << (int)result);
	return result;
}

void V9990::syncAtNextLine(V9990SyncType type, const EmuTime& time)
{
	int line = getUCTicksThisFrame(time) / V9990DisplayTiming::UC_TICKS_PER_LINE;
	int ticks = (line + 1) * V9990DisplayTiming::UC_TICKS_PER_LINE;
	EmuTime nextTime = frameStartTime + ticks;
	setSyncPoint(nextTime, type);
}

void V9990::writeRegister(byte reg, byte val, const EmuTime& time)
{
	// Found this table by writing 0xFF to a register and reading
	// back the value (only works for read/write registers)
	static const byte regWriteMask[32] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0x87, 0xFF, 0x83, 0x0F, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xDF, 0x07, 0xFF, 0xFF, 0xC1, 0x07,
		0x3F, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};

	//PRT_DEBUG("[" << time << "] "
	//	  "V9990::writeRegister - reg=0x" << std::hex << int(reg) <<
	//	                        " val=0x" << std::hex << int(val));

	assert(reg < 64);
	if ((regAccess[reg] == NO_ACCESS) || (regAccess[reg] == RD_ONLY)) {
		// register not writable
		return;
	}
	if (reg >= CMD_PARAM_SRC_ADDRESS_0) {
		cmdEngine->setCmdReg(reg, val, time);
		return;
	}

	val &= regWriteMask[reg];

	byte change = regs[reg] ^ val;
	// This optimization is not valid for the vertical scroll registers
	// TODO is this optimization still useful for other registers?
	//if (!change) return;

	// Perform additional tasks before new value becomes active
	switch (reg) {
		case SCREEN_MODE_0:
		case SCREEN_MODE_1:
			// TODO verify this on real V9990
			syncAtNextLine(V9990_SET_MODE, time);
			break;
		case CONTROL:
			if (change & 0x80) {
				syncAtNextLine(V9990_SET_BLANK, time);
			}
			break;
		case PALETTE_CONTROL:
			renderer->setColorMode(getColorMode(val), time);
			break;
		case BACK_DROP_COLOR:
			renderer->updateBackgroundColor(val & 63, time);
			break;
		case SCROLL_CONTROL_AY0:
			renderer->updateScrollAYLow(time);
			break;
		case SCROLL_CONTROL_AY1:
			renderer->updateScrollAYHigh(time);
			break;
		case SCROLL_CONTROL_AX0:
		case SCROLL_CONTROL_AX1:
			renderer->updateScrollAX(time);
			break;
		case SCROLL_CONTROL_BY0:
		case SCROLL_CONTROL_BY1:
			renderer->updateScrollBY(time);
			break;
		case SCROLL_CONTROL_BX0:
		case SCROLL_CONTROL_BX1:
			renderer->updateScrollBX(time);
			break;
	}
	// commit the change
	regs[reg] = val;

	// Perform additional tasks after new value became active
	switch (reg) {
		case INTERRUPT_0:
			if (pendingIRQs & val) {
				irq.set();
			} else {
				irq.reset();
			}
			break;
		case INTERRUPT_1:
		case INTERRUPT_2:
		case INTERRUPT_3:
			scheduleHscan(time);
			break;
	}
}

void V9990::writePaletteRegister(byte reg, byte val, const EmuTime& time)
{
	switch (reg & 3) {
		case 0: val &= 0x9F; break;
		case 1: val &= 0x1F; break;
		case 2: val &= 0x1F; break;
		case 3: val  = 0x00; break;
	}
	palette[reg] = val;
	reg &= ~3;
	byte index = reg / 4;
	renderer->updatePalette(index, palette[reg + 0], palette[reg + 1],
	                        palette[reg + 2], time);
	if (index == regs[BACK_DROP_COLOR]) {
		renderer->updateBackgroundColor(index, time);
	}
}

void V9990::getPalette(int index, byte& r, byte& g, byte& b)
{
	r = palette[4 * index + 0];
	g = palette[4 * index + 1];
	b = palette[4 * index + 2];
}

void V9990::createRenderer(const EmuTime& time)
{
	assert(!renderer.get());
	Display& display = getMotherBoard().getDisplay();
	renderer.reset(RendererFactory::createV9990Renderer(*this, display));
	renderer->reset(time);
}

void V9990::frameStart(const EmuTime& time)
{
	// Update setings that are fixed at the start of a frame
	palTiming = regs[SCREEN_MODE_1] & 0x08;
	setVerticalTiming();
	status ^= 0x02; // flip EO bit

	frameStartTime.advance(time);

	// schedule next VSYNC
	setSyncPoint(
		frameStartTime + V9990DisplayTiming::getUCTicksPerFrame(palTiming),
		V9990_VSYNC);

	// schedule DISPLAY_START
	int topBorder = getVerticalTiming().blank + getVerticalTiming().border1;
	setSyncPoint(
	        frameStartTime + topBorder * V9990DisplayTiming::UC_TICKS_PER_LINE,
	        V9990_DISPLAY_START);

	// schedule VSCAN
	int bottomBorder = topBorder + getVerticalTiming().display;
	setSyncPoint(
	        frameStartTime + bottomBorder * V9990DisplayTiming::UC_TICKS_PER_LINE,
	        V9990_VSCAN);

	renderer->frameStart(time);
}

void V9990::raiseIRQ(IRQType irqType)
{
	pendingIRQs |= irqType;
	if (pendingIRQs & regs[INTERRUPT_0]) {
		irq.set();
	}
}

void V9990::setHorizontalTiming()
{
	switch (mode) {
	case P1: case P2:
	case B1: case B3: case B7:
		horTiming = &V9990DisplayTiming::lineMCLK;
		break;
	case B0: case B2: case B4:
		horTiming = &V9990DisplayTiming::lineXTAL;
	case B5: case B6:
		break;
	default:
		assert(false);
	}
}

void V9990::setVerticalTiming()
{
	switch (mode) {
	case P1: case P2:
	case B1: case B3: case B7:
		verTiming = isPalTiming()
		          ? &V9990DisplayTiming::displayPAL_MCLK
		          : &V9990DisplayTiming::displayNTSC_MCLK;
		break;
	case B0: case B2: case B4:
		verTiming = isPalTiming()
		          ? &V9990DisplayTiming::displayPAL_XTAL
		          : &V9990DisplayTiming::displayNTSC_XTAL;
	case B5: case B6:
		break;
	default:
		assert(false);
	}
}

V9990ColorMode V9990::getColorMode(byte pal_ctrl) const
{
	V9990ColorMode mode = INVALID_COLOR_MODE;

	if (!regs[SCREEN_MODE_0] & 0x80) {
		mode = BP4;
	} else {
		switch (regs[SCREEN_MODE_0] & 0x03) {
			case 0x00: mode = BP2; break;
			case 0x01: mode = BP4; break;
			case 0x02:
				switch (pal_ctrl & 0xC0) {
					case 0x00: mode = BP6; break;
					case 0x40: mode = BD8; break;
					case 0x80: mode = BYJK; break;
					case 0xC0: mode = BYUV; break;
					default: assert(false); break;
				}
				break;
			case 0x03: mode = BD16; break;
			default: assert(false); break;
		}
	}

	// TODO Check
	if (mode == INVALID_COLOR_MODE) mode = BP4;
	return mode;
}

V9990ColorMode V9990::getColorMode() const
{
	return getColorMode(regs[PALETTE_CONTROL]);
}

void V9990::calcDisplayMode()
{
	mode = INVALID_DISPLAY_MODE;
	switch (regs[SCREEN_MODE_0] & 0xC0) {
		case 0x00:
			mode = P1;
			break;
		case 0x40:
			mode = P2;
			break;
		case 0x80:
			if(status & 0x04) { // MCLK timing
				switch(regs[SCREEN_MODE_0] & 0x30) {
				case 0x00: mode = B0; break;
				case 0x10: mode = B2; break;
				case 0x20: mode = B4; break;
				case 0x30: mode = INVALID_DISPLAY_MODE; break;
				default: assert(false);
				}
			} else { // XTAL1 timing
				switch(regs[SCREEN_MODE_0] & 0x30) {
				case 0x00: mode = B1; break;
				case 0x10: mode = B3; break;
				case 0x20: mode = B7; break;
				case 0x30: mode = INVALID_DISPLAY_MODE; break;
				}
			}
			break;
		case 0xC0:
			mode = INVALID_DISPLAY_MODE;
			break;
	}

	// TODO Check
	if (mode == INVALID_DISPLAY_MODE) mode = P1;

	setHorizontalTiming();
}

void V9990::scheduleHscan(const EmuTime& time)
{
	// remove pending HSCAN, if any
	if (hScanSyncTime > time) {
		removeSyncPoint(V9990_HSCAN);
		hScanSyncTime = time;
	}

	if (pendingIRQs & HOR_IRQ) {
		// flag already set, no need to schedule
		return;
	}

	int ticks = frameStartTime.getTicksTill(time);
	int offset;
	if (regs[INTERRUPT_2] & 0x80) {
		// every line
		offset = ticks - (ticks % V9990DisplayTiming::UC_TICKS_PER_LINE);
	} else {
		int line = regs[INTERRUPT_1] + 256 * (regs[INTERRUPT_2] & 3) +
		           getVerticalTiming().blank + getVerticalTiming().border1;
		offset = line * V9990DisplayTiming::UC_TICKS_PER_LINE;
	}
	int mult = (status & 0x04) ? 3 : 2; // MCLK / XTAL1
	offset += (regs[INTERRUPT_3] & 0x0F) * 64 * mult;
	if (offset <= ticks) {
		offset += V9990DisplayTiming::getUCTicksPerFrame(palTiming);
	}

	hScanSyncTime = frameStartTime + offset;
	setSyncPoint(hScanSyncTime, V9990_HSCAN);
}

} // namespace openmsx

