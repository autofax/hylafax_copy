/*	$Id: Class1Recv.c++ 1133 2012-12-26 06:51:02Z faxguy $ */
/*
 * Copyright (c) 1990-1996 Sam Leffler
 * Copyright (c) 1991-1996 Silicon Graphics, Inc.
 * HylaFAX is a trademark of Silicon Graphics
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * EIA/TIA-578 (Class 1) Modem Driver.
 *
 * Receive protocol.
 */
#include <stdio.h>
#include <sys/time.h>
#include "Class1.h"
#include "ModemConfig.h"
#include "HDLCFrame.h"
#include "StackBuffer.h"		// XXX

#include "t.30.h"
#include "Sys.h"
#include "config.h"
#include "FaxParams.h"

/*
 * Tell the modem to answer the phone.  We override
 * this method so that we can force the terminal's
 * flow control state to be setup to our liking.
 */
CallType
Class1Modem::answerCall(AnswerType type, fxStr& emsg, const char* number)
{
    // Reset modemParams.br to non-V.34 settings.  If V.8 handshaking
    // succeeds, then it will be changed again.
    modemParams.br = nonV34br;

    if (flowControl == FLOW_XONXOFF)
	setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_FLUSH);
    return ClassModem::answerCall(type, emsg, number);
}

/*
 * Process an answer response from the modem.
 * Since some Class 1 modems do not give a connect
 * message that distinguishes between DATA and FAX,
 * we override the default handling of "CONNECT"
 * message here to force the high level code to
 * probe further.
 */
const AnswerMsg*
Class1Modem::findAnswer(const char* s)
{
    static const AnswerMsg answer[2] = {
    { "CONNECT ", 8,
      FaxModem::AT_NOTHING, FaxModem::OK, FaxModem::CALLTYPE_DATA },
    { "CONNECT",  7,
      FaxModem::AT_NOTHING, FaxModem::OK, FaxModem::CALLTYPE_UNKNOWN },
    };
    return strneq(s, answer[0].msg, answer[0].len) ? &answer[0] :
	   strneq(s, answer[1].msg, answer[1].len) ? &answer[1] :
	      FaxModem::findAnswer(s);
}

/*
 * Begin the receive protocol.
 */
bool
Class1Modem::recvBegin(FaxSetup* setupinfo, fxStr& emsg)
{
    setInputBuffering(false);
    prevPage = 0;				// no previous page received
    pageGood = false;				// quality of received page
    messageReceived = false;			// expect message carrier
    recvdDCN = false;				// haven't seen DCN
    lastPPM = FCF_DCN;				// anything will do
    sendCFR = false;				// TCF was not received
    lastMCF = 0;				// no MCF heard yet
    capsUsed = 0;				// no DCS or CTC seen yet
    dataSent = 0;				// start with a clean slate...
    dataMissed = 0;				// unfortunately, this will reset after EOM
    senderSkipsV29 = false;
    senderHasV17Trouble = false;

    if (setupinfo) {
	senderSkipsV29 = setupinfo->senderSkipsV29;
	senderHasV17Trouble = setupinfo->senderHasV17Trouble;
    }

    fxStr nsf;
    encodeNSF(nsf, HYLAFAX_VERSION);

    if (useV34 && !gotCTRL) waitForDCEChannel(true);	// expect control channel

    FaxParams dis = modemDIS();

    if (senderSkipsV29 && senderHasV17Trouble) {
	dis.setBit(FaxParams::BITNUM_SIGRATE_14, false);	// disable V.17 support
	protoTrace("This sender skips V.29 and has trouble with V.17.  Concealing V.17 support.");
    }
    if (conf.class1RestrictPoorSenders && setupinfo && setupinfo->senderDataSent && 
	setupinfo->senderDataMissed * 100 / setupinfo->senderDataSent > conf.class1RestrictPoorSenders) {
	dis.setBit(FaxParams::BITNUM_VR_FINE, false);	// disable fine resolution support
	dis.setBit(FaxParams::BITNUM_VR_R8, false);	// disable superfine resolution support
	dis.setBit(FaxParams::BITNUM_VR_300X300, false);// disable 300x300 dpi support
	dis.setBit(FaxParams::BITNUM_VR_R16, false);	// disable hyperfine resolution support
	dis.setBit(FaxParams::BITNUM_JPEG, false);	// disable JPEG support
	dis.setBit(FaxParams::BITNUM_FULLCOLOR, false);	// disable color JPEG support
	protoTrace("This sender exhibits poor call audio quality.  Concealing resolution and color support.");
    }

    bool ok = FaxModem::recvBegin(setupinfo, emsg) && recvIdentification(
	0, fxStr::null,
	0, fxStr::null,
	FCF_NSF|FCF_RCVR, nsf,
	FCF_CSI|FCF_RCVR, lid,
	FCF_DIS|FCF_RCVR, dis,
	conf.class1RecvIdentTimer, false, emsg);
    if (setupinfo) {
	/*
	 * Update FaxMachine info...
	 */
	setupinfo->senderSkipsV29 = senderSkipsV29;
	setupinfo->senderHasV17Trouble = senderHasV17Trouble;
	setupinfo->senderDataSent = dataSent;
	setupinfo->senderDataMissed = dataMissed;
    }
    return (ok);
}

/*
 * Begin the receive protocol after an EOM signal.
 */
bool
Class1Modem::recvEOMBegin(FaxSetup* setupinfo, fxStr& emsg)
{
    /*
     * We must raise the transmission carrier to mimic the state following ATA.
     */
    if (!useV34) {
	pause(conf.t2Timer);	// T.30 Fig 5.2B requires T2 to elapse
	if (!(atCmd(thCmd, AT_NOTHING) && atResponse(rbuf, 0) == AT_CONNECT)) {
	    emsg = "Failure to raise V.21 transmission carrier. {E101}";
	    protoTrace(emsg);
	    return (false);
	}
    }
    return Class1Modem::recvBegin(setupinfo, emsg);
}

/*
 * Transmit local identification and wait for the
 * remote side to respond with their identification.
 */
bool
Class1Modem::recvIdentification(
    u_int f1, const fxStr& pwd,
    u_int f2, const fxStr& addr,
    u_int f3, const fxStr& nsf,
    u_int f4, const fxStr& id,
    u_int f5, FaxParams& dics,
    u_int timer, bool notransmit, fxStr& emsg)
{
    u_int t1 = howmany(timer, 1000);		// in seconds
    time_t start = Sys::now();
    HDLCFrame frame(conf.class1FrameOverhead);
    bool framesSent = false;
    u_int onhooks = 0;

    emsg = "No sender protocol (T.30 T1 timeout) {E102}";
    if (!notransmit) {
	/*
	 * Transmit (PWD) (SUB) (CSI) DIS frames when the receiving
	 * station or (PWD) (SEP) (CIG) DTC when initiating a poll.
	 */
	if (f1) {
	    startTimeout(7550);
	    framesSent = sendFrame(f1, pwd, false);
	    stopTimeout("sending PWD frame");
	} else if (f2) {
	    startTimeout(7550);
	    framesSent = sendFrame(f2, addr, false);
	    stopTimeout("sending SUB/SEP frame");
	} else if (f3) {
	    startTimeout(7550);
	    framesSent = sendFrame(f3, (const u_char*)HYLAFAX_NSF, nsf, false);
	    stopTimeout("sending NSF frame");
	} else {
	    startTimeout(7550);
	    framesSent = sendFrame(f4, id, false);
	    stopTimeout("sending CSI/CIG frame");
	}
    }
    for (;;) {
	if (framesSent && !notransmit) {
	    if (f1) {
		startTimeout(7550);
		framesSent = sendFrame(f2, addr, false);
		stopTimeout("sending SUB/SEP frame");
	    }
	    if (framesSent && f2) {
		startTimeout(7550);
		framesSent = sendFrame(f3, (const u_char*)HYLAFAX_NSF, nsf, false);
		stopTimeout("sending NSF frame");
	    }
	    if (framesSent && f3) {
		startTimeout(7550);
		framesSent = sendFrame(f4, id, false);
		stopTimeout("sending CSI/CIG frame");
	    }
	    if (framesSent) {
		startTimeout(7550);
		framesSent = sendFrame(f5, dics);
		stopTimeout("sending DIS/DCS frame");
	    }
	}
	if (framesSent || notransmit) {
	    /*
	     * Wait for a response to be received.  We wait T2
	     * rather than T4 due to empirical evidence for that need.
	     */
	    if (recvFrame(frame, FCF_RCVR, conf.t2Timer, false, true, false)) {
		do {
		    /*
		     * Verify a DCS command response and, if
		     * all is correct, receive phasing/training.
		     */
		    bool gotframe = true;
		    while (gotframe) {
			if (!recvDCSFrames(frame)) {
			    switch (frame.getFCF()) {
				case FCF_DCN:
				    emsg = "RSPREC error/got DCN (sender abort) {E103}";
				    recvdDCN = true;
				    return (false);
				case FCF_MPS:
				case FCF_EOP:
				case FCF_EOM:
				    if (!useV34 && !switchingPause(emsg)) return (false);
				    transmitFrame(signalSent);
				    traceFCF("RECV send", (u_char) signalSent[2]);
				    break;
				case FCF_FTT:
				    /* probably just our own echo */
				    break;
				case FCF_CRP:
				    /* do nothing here, just let us repeat NSF, CSI, DIS */
				    break;
				default:	// XXX DTC/DIS not handled
				    emsg = "RSPREC invalid response received {E104}";
				    break;
			    }
			    break;
			}
			gotframe = false;
			if (recvTraining()) {
			    emsg = "";
			    return (true);
			} else {
			    if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT,0)) {
				// It's unclear if we are in "COMMAND REC" or "RESPONSE REC" mode,
				// but since we are already detecting the carrier, wait the longer.
				gotframe = recvFrame(frame, FCF_RCVR, conf.t2Timer, true, false, false);
				if (!gotframe && !frame.getLength() && lastResponse == AT_NOCARRIER) {
				    /*
				     * The modem may have incorrectly detected V.21 HDLC.
				     * The TCF signal is yet to come.  So, try again.
				     */
				    if (recvTraining()) {
					emsg = "";
					return (true);
				    }
				    if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT,0)) {
					// It's unclear if we are in "COMMAND REC" or "RESPONSE REC" mode,
					// but since we are already detecting the carrier, wait the longer.
					gotframe = recvFrame(frame, FCF_RCVR, conf.t2Timer, true, true, false);
				    }
				}
				lastResponse = AT_NOTHING;
			    }
			}
		    }
		    if (gotframe) break;	// where recvDCSFrames fails without DCN
		    emsg = "Failure to train modems {E105}";
		    /*
		     * Reset the timeout to insure the T1 timer is
		     * used.  This is done because the adaptive answer
		     * strategy may setup a shorter timeout that's
		     * used to wait for the initial identification
		     * frame.  If we get here then we know the remote
		     * side is a fax machine and so we should wait
		     * the full T1 timeout, as specified by the protocol.
		     */
		    t1 = howmany(conf.t1Timer, 1000);
		} while (recvFrame(frame, FCF_RCVR, conf.t2Timer, false, true, false));
	    }
	}
	if (gotEOT && ++onhooks > conf.class1HookSensitivity) {
	    emsg = "RSPREC error/got EOT {E106}";
	    return (false);
	}
	/*
	 * We failed to send our frames or failed to receive
	 * DCS from the other side.  First verify there is
	 * time to make another attempt...
	 */
	if ((u_int) Sys::now()-start >= t1)
	    return (false);
	if (frame.getFCF() != FCF_CRP) {
	    /*
	     * Historically we waited "Class1TrainingRecovery" (1500 ms)
	     * at this point to try to try to avoid retransmitting while
	     * the sender is also transmitting.  Sometimes it proved to be
	     * a faulty approach.  Really what we're trying to do is to
	     * not be transmitting at the same time as the other end is.
	     * The best way to do that is to make sure that there is
	     * silence on the line, and  we do that with Class1SwitchingCmd.
	     */
	    if (!useV34 && !switchingPause(emsg)) {
		return (false);
	    }
	}
	if (!notransmit) {
	    /*
	     * Retransmit ident frames.
	     */
	    if (f1)
		framesSent = transmitFrame(f1, pwd, false);
	    else if (f2)
		framesSent = transmitFrame(f2, addr, false);
	    else if (f3)
		framesSent = transmitFrame(f3, (const u_char*)HYLAFAX_NSF, nsf, false);
	    else
		framesSent = transmitFrame(f4, id, false);
	}
    }
    return (false);
}

/*
 * Receive DCS preceded by any optional frames.
 * Although technically this is "RESPONSE REC",
 * we wait T2 due to empirical evidence of that need.
 */
bool
Class1Modem::recvDCSFrames(HDLCFrame& frame)
{
    fxStr s;
    do {
	traceFCF("RECV recv", frame.getFCF());
	switch (frame.getFCF()) {
	case FCF_PWD:
	    recvPWD(decodePWD(s, frame));
	    break;
	case FCF_SUB:
	    recvSUB(decodePWD(s, frame));
	    break;
	case FCF_TSI:
	    recvTSI(decodeTSI(s, frame));
	    break;
	case FCF_DCS:
	    if (frame.getFrameDataLength() < 4) return (false);	// minimum acceptable DCS frame size
	    processDCSFrame(frame);
	    break;
	case FCF_DCN:
	    gotEOT = true;
	    recvdDCN = true;
	    break;
	}
	/*
	 * Sometimes echo is bad enough that we hear ourselves.  So if we hear DIS, we're probably
	 * hearing ourselves.  Just ignore it and listen again.
	 */
    } while (!recvdDCN && (frame.moreFrames() || frame.getFCF() == FCF_DIS) && recvFrame(frame, FCF_RCVR, conf.t2Timer));
    return (frame.isOK() && frame.getFCF() == FCF_DCS);
}

/*
 * Receive training and analyze TCF.
 */
bool
Class1Modem::recvTraining()
{
    if (useV34) {
	sendCFR = true;
	return (true);
    }
    /*
     * It is possible (and with some modems likely) that the sending
     * system has not yet dropped its V.21 carrier because the modem may
     * simply signal OK when the HDLC frame is received completely and not
     * not wait for the carrier drop to occur.  We don't follow the strategy 
     * documented in T.31 Appendix II.1 about issuing another +FRH and 
     * waiting for NO CARRIER because it's possible that the sender does not
     * send enough V.21 HDLC after the last frame to make that work.
     *
     * The remote has to wait 75 +/- 20 ms after DCS before sending us TCF 
     * as dictated by T.30 Chapter 5, Note 3.  If we have a modem that gives
     * us an OK after DCS before the sender actually drops the carrier, then
     * the best approach will be to simply look for silence with AT+FRS=1.
     * Unfortunately, +FRS is not supported on all modems, and so when they
     * need it, they will have to simply use a <delay:n> or possibly use
     * a different command sequence.
     *
     */
    if (!atCmd(conf.class1TCFRecvHackCmd, AT_OK)) {
	return (false);
    }

    protoTrace("RECV training at %s %s",
	modulationNames[curcap->mod],
	Class2Params::bitRateNames[curcap->br]);
    HDLCFrame buf(conf.class1FrameOverhead);
    bool ok = recvTCF(curcap->value, buf, frameRev, conf.class1TCFRecvTimeout);
    if (curcap->mod == V17) senderHasV17Trouble = !ok;	// if TCF failed
    if (ok) {					// check TCF data
	u_int n = buf.getLength();
	u_int nonzero = 0;
	u_int zerorun = 0;
	u_int i = 0;
	/*
	 * Skip any initial non-zero training noise.
	 */
	while (i < n && buf[i] != 0)
	    i++;
	/*
	 * Determine number of non-zero bytes and
	 * the longest zero-fill run in the data.
	 */
	if (i < n) {
	    while (i < n) {
		u_int j;
		for (; i < n && buf[i] != 0; i++)
		    nonzero++;
		for (j = i; j < n && buf[j] == 0; j++)
		    ;
		if (j-i > zerorun)
		    zerorun = j-i;
		i = j;
	    }
	} else {
	    /*
	     * There was no non-zero data.
	     */
	    nonzero = n;
	}
	/*
	 * Our criteria for accepting is that there must be
	 * no more than 10% non-zero (bad) data and the longest
	 * zero-run must be at least at least 2/3'rds of the
	 * expected TCF duration.  This is a hack, but seems
	 * to work well enough.  What would be better is to
	 * anaylze the bit error distribution and decide whether
	 * or not we would receive page data with <N% error,
	 * where N is probably ~5.  If we had access to the
	 * modem hardware, the best thing that we could probably
	 * do is read the Eye Quality register (or similar)
	 * and derive an indicator of the real S/N ratio.
	 */
	u_int fullrun = params.transferSize(TCF_DURATION);
	u_int minrun = params.transferSize(conf.class1TCFMinRun);
	if (params.ec != EC_DISABLE && conf.class1TCFMinRunECMMod > 0) {
	    /*
	     * When using ECM it may not be wise to fail TCF so easily
	     * as retransmissions can compensate for data corruption.
	     * For example, if there is a regular disturbance in the 
	     * audio every second that will cause TCFs to fail, but where
	     * the majority of the TCF data is "clean", then it will
	     * likely be better to pass TCF more easily at the faster
	     * rate rather than letting things slow down where the 
	     * disturbance will only cause slower retransmissions (and
	     * more of them, too).
	     */
	    minrun /= conf.class1TCFMinRunECMMod;
	}
	nonzero = (100*nonzero) / (n == 0 ? 1 : n);
	protoTrace("RECV: TCF %u bytes, %u%% non-zero, %u zero-run",
	    n, nonzero, zerorun);
	if (zerorun < fullrun && nonzero > conf.class1TCFMaxNonZero) {
	    protoTrace("RECV: reject TCF (too many non-zero, max %u%%)",
		conf.class1TCFMaxNonZero);
	    ok = false;
	}
	if (zerorun < minrun) {
	    protoTrace("RECV: reject TCF (zero run too short, min %u)", minrun);
	    ok = false;
	}
	if (!wasTimeout()) {
	    /*
	     * We expect the message carrier to drop.  However, some senders will
	     * transmit garbage after we see <DLE><ETX> but before we see NO CARRIER.
	     */
	    time_t nocarrierstart = Sys::now();
	    bool gotnocarrier = false;
	    do {
		gotnocarrier = waitFor(AT_NOCARRIER, 2*1000);
	    } while (!gotnocarrier && Sys::now() < (nocarrierstart + 5));
	}
    } else {
	// the CONNECT is waited for later...
	if (lastResponse == AT_FCERROR && atCmd(rhCmd, AT_NOTHING)) lastResponse = AT_FRH3;
	if (lastResponse == AT_FRH3) return (false);	// detected V.21 carrier
    }
    /*
     * Send training response; we follow the spec
     * by delaying 75ms before switching carriers.
     */
    fxStr emsg;
    if (!switchingPause(emsg)) return (false);
    if (ok) {
	/*
	 * Send CFR later so that we can cancel
	 * session by DCN if it's needed. 
	 */
	sendCFR = true;
	protoTrace("TRAINING succeeded");
    } else {
	transmitFrame(FCF_FTT|FCF_RCVR);
	sendCFR = false;
	protoTrace("TRAINING failed");
    }
    return (ok);
}

void
Class1Modem::processNewCapabilityUsage()
{
    capsUsed |= BIT(curcap->num);		// add this modulation+bitrate to the used list
    if ((capsUsed & 0x94) == 0x94) {
	senderSkipsV29 = ((capsUsed & 0xDC) == 0x94);
    }
}

/*
 * Process a received DCS frame.
 */
void
Class1Modem::processDCSFrame(const HDLCFrame& frame)
{
    FaxParams dcs_caps = frame.getDIS();			// NB: really DCS

    if (dcs_caps.isBitEnabled(FaxParams::BITNUM_FRAMESIZE_DCS)) frameSize = 64;
    else frameSize = 256;
    params.setFromDCS(dcs_caps);
    if (useV34) params.br = primaryV34Rate-1;
    else {
	curcap = findSRCapability((dcs_caps.getByte(1)<<8)&DCS_SIGRATE, recvCaps);
	processNewCapabilityUsage();
    }
    setDataTimeout(60, params.br);
    recvDCS(params);				// announce session params
}

const u_int Class1Modem::modemPPMCodes[8] = {
    0,			// 0
    PPM_EOM,		// FCF_EOM+FCF_PRI_EOM
    PPM_MPS,		// FCF_MPS+FCF_PRI_MPS
    0,			// 3
    PPM_EOP,		// FCF_EOP+FCF_PRI_EOP
    0,			// 5
    0,			// 6
    0,			// 7
};

/*
 * Receive a page of data.
 *
 * This routine is called after receiving training or after
 * sending a post-page response in a multi-page document.
 */
bool
Class1Modem::recvPage(TIFF* tif, u_int& ppm, fxStr& emsg, const fxStr& id)
{
    if (lastPPM == FCF_MPS && prevPage) {
	/*
	 * Resume sending HDLC frame (send data)
	 * The carrier is already raised.  Thus we
	 * use sendFrame() instead of transmitFrame().
	 */
	if (pageGood) {
	    startTimeout(7550);
	    (void) sendFrame((sendERR ? FCF_ERR : FCF_MCF)|FCF_RCVR);
	    stopTimeout("sending HDLC frame");
	    lastMCF = Sys::now();
	} else if (conf.badPageHandling == FaxModem::BADPAGE_RTNSAVE) {
	    startTimeout(7550);
	    (void) sendFrame(FCF_RTN|FCF_RCVR);
	    stopTimeout("sending HDLC frame");
	    FaxParams dis = modemDIS();
	    if (!recvIdentification(0, fxStr::null, 0, fxStr::null, 
		0, fxStr::null, 0, fxStr::null, 0, dis,
		conf.class1RecvIdentTimer, true, emsg)) {
		return (false);
	    }
	}
    }

    time_t t2end = 0;
    signalRcvd = 0;
    sendERR = false;
    gotCONNECT = true;

    do {
	ATResponse rmResponse = AT_NOTHING;
	long timer = conf.t2Timer;
	if (!messageReceived) {
	    if (sendCFR ) {
		transmitFrame(FCF_CFR|FCF_RCVR);
		sendCFR = false;
	    }
	    pageGood = pageStarted = false;
	    resetLineCounts();		// in case we don't make it to initializeDecoder
	    recvSetupTIFF(tif, group3opts, FILLORDER_LSB2MSB, id);
	    rmResponse = AT_NOTHING;
	    if (params.ec != EC_DISABLE || useV34) {
		pageGood = recvPageData(tif, emsg);
		messageReceived = true;
		prevPage++;
	    } else {
		bool retryrmcmd;
		int rmattempted = 0;
		do {
		    retryrmcmd = false;
		    /*
		     * Look for message carrier and receive Phase C data.
		     */
		    /*
		     * Same reasoning here as before receiving TCF.
		     */
		    if (!atCmd(conf.class1MsgRecvHackCmd, AT_OK)) {
			emsg = "Failure to receive silence (synchronization failure). {E100}";
			return (false);
		    }
		    /*
		     * Set high speed carrier & start receive.  If the
		     * negotiated modulation technique includes short
		     * training, then we use it here (it's used for all
		     * high speed carrier traffic other than the TCF).
		     *
		     * Timing here is very critical.  It is more "tricky" than timing
		     * for AT+FRM for TCF because unlike with TCF, where the direction
		     * of communication doesn't change, here it does change because 
		     * we just sent CFR but now have to do AT+FRM.  In practice, if we 
		     * issue AT+FRM after the sender does AT+FTM then we'll get +FCERROR.
		     * Using Class1MsgRecvHackCmd often only complicates the problem.
		     * If the modem doesn't drop its transmission carrier (OK response
		     * following CFR) quickly enough, then we'll see more +FCERROR.
		     */
		    fxStr rmCmd(curcap[HasShortTraining(curcap)].value, rmCmdFmt);
		    u_short attempts = 0;
		    do {
			(void) atCmd(rmCmd, AT_NOTHING);
			rmResponse = atResponse(rbuf, conf.class1RMPersistence ? conf.t2Timer + 2900 : conf.t2Timer - 2900);
		    } while ((rmResponse == AT_NOTHING || rmResponse == AT_FCERROR) && ++attempts < conf.class1RMPersistence);
		    if (rmResponse == AT_CONNECT) {
			/*
			 * We don't want the AT+FRM=n command to get buffered,
			 * so buffering and flow control must be done after CONNECT.
			 * Flushing now would be a mistake as data may already be
			 * in the buffer.
			 */
			setInputBuffering(true);
			if (flowControl == FLOW_XONXOFF)
			    (void) setXONXOFF(FLOW_NONE, FLOW_XONXOFF, ACT_NOW);
			/*
			 * The message carrier was recognized;
			 * receive the Phase C data.
			 */
			protoTrace("RECV: begin page");
			pageGood = recvPageData(tif, emsg);
			protoTrace("RECV: end page");
			if (!wasTimeout()) {
			    /*
			     * The data was received correctly, wait patiently
			     * for the modem to signal carrier drop.  Some senders
			     * may send a lot of garbage after RTC, so be patient.
			     */
			    time_t nocarrierstart = Sys::now();
			    do {
				messageReceived = waitFor(AT_NOCARRIER, 60*1000);
			    } while (!messageReceived && Sys::now() < (nocarrierstart + 60));
			    if (messageReceived)
				prevPage++;
		            timer = BIT(curcap->br) & BR_ALL ? 273066 / (curcap->br+1) : conf.t2Timer;	// wait longer for PPM (estimate 80KB)
			}
		    } else {
			if (wasTimeout()) {
			    abortReceive();		// return to command mode
			    setTimeout(false);
			}
			bool getframe = false;
			long wait = BIT(curcap->br) & BR_ALL ? 273066 / (curcap->br+1) : conf.t2Timer;
			if (rmResponse == AT_FRH3) getframe = waitFor(AT_CONNECT, 0);
			else if (rmResponse != AT_NOCARRIER && rmResponse != AT_ERROR) getframe = atCmd(rhCmd, AT_CONNECT, wait);	// wait longer
			if (getframe) {
			    HDLCFrame frame(conf.class1FrameOverhead);
			    if (recvFrame(frame, FCF_RCVR, conf.t2Timer, true, false)) {
				traceFCF("RECV recv", frame.getFCF());
				signalRcvd = frame.getFCF();
				messageReceived = true;
			    } else {
				/*
				 * V.21 HDLC was detected and then the carrier was lost without
				 * receiving any data.  It's possible that the modem erred in 
				 * its detection of the high-speed carrier.  But, it's also 
				 * possible that echo of our CFR was detected or that there is 
				 * another receiver on the line (another fax machine sharing the 
				 * line on the send-side), and we heard them.  Often we can still 
				 * acquire the high-speed carrier if we just re-issue AT+FRM=n.
				 */
				if (lastResponse == AT_NOCARRIER) retryrmcmd = true;
			    }
			}
		    }
		} while (retryrmcmd && ++rmattempted < 2);
	    }
	    if (signalRcvd != 0) {
		if (flowControl == FLOW_XONXOFF)
		    (void) setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_DRAIN);
		setInputBuffering(false);
	    }
	    if (!messageReceived && rmResponse != AT_FCERROR && rmResponse != AT_FRH3) {
		if (rmResponse != AT_ERROR) {
		    /*
		     * One of many things may have happened:
		     * o if we lost carrier, then some modems will return
		     *   AT_NOCARRIER or AT_EMPTYLINE in response to the
		     *   AT+FRM request.
		     * o otherwise, there may have been a timeout receiving
		     *   the message data, or there was a timeout waiting
		     *   for the carrier to drop.
		     */
		    if (!wasTimeout()) {
			/*
			 * We found the wrong carrier, which means that there
			 * is an HDLC frame waiting for us--in which case it
			 * should get picked up below.
			 */
			break;
		    }
		    /*
		     * The timeout expired - thus we missed the carrier either
		     * raising or dropping.
		     */
		    abortReceive();		// return to command state
		    break;
		} else {
		    /*
		     * Some modems respond ERROR instead +FCERROR on wrong carrier
		     * and not return to command state.
		     */
		    abortReceive();		// return to command state
		}
	    }
	}
	/*
	 * T.30 says to process operator intervention requests
	 * here rather than before the page data is received.
	 * This has the benefit of not recording the page as
	 * received when the post-page response might need to
	 * be retransmited.
	 */
	if (abortRequested()) {
	    // XXX no way to purge TIFF directory
	    emsg = "Receive aborted due to operator intervention {E301}";
	    return (false);
	}

	/*
	 * Acknowledge PPM from ECM protocol.
	 */
	HDLCFrame frame(conf.class1FrameOverhead);
	bool ppmrcvd;
	if (signalRcvd != 0) {
	    ppmrcvd = true;
	    lastPPM = signalRcvd;
	    for (u_int i = 0; i < frameRcvd.length(); i++) frame.put(frameRcvd[i]);
	    frame.setOK(true);
	} else {
	    gotCONNECT = false;
	    u_short recvFrameCount = 0;
	    time_t ppmstart = Sys::now();
	    do {
		/*
		 * Some modems will report CONNECT erroniously on high-speed Phase C data.
		 * Then they will time-out on HDLC data presentation instead of dumping
		 * garbage or quickly resulting ERROR.  So we give instances where CONNECT
		 * occurs a bit more tolerance here...
		 */
		ppmrcvd = recvFrame(frame, FCF_RCVR, timer);
	    } while (!ppmrcvd && gotCONNECT && wasTimeout() && !gotEOT && ++recvFrameCount < 3);
	    if (ppmrcvd) lastPPM = frame.getFCF();
	    /*
	     * To combat premature carrier loss leading to MCF instead of RTN on short/partial pages,
	     * We started a timer above and measured the time it took to receive PPM.  If longer
	     * longer than 5 seconds, and if we did not see RTC, then we assume that premature
	     * carrier loss occurred and set pageGood to false.
	     */
	    if (Sys::now() - ppmstart > 5 && !getSeenRTC()) {
		protoTrace("RECV detected premature Phase C carrier loss.");
		pageGood = false;
	    }
	}
	/*
	 * Do command received logic.
	 */
	if (ppmrcvd) {
	    switch (lastPPM) {
	    case FCF_DIS:			// XXX no support
		if (!pageGood) recvResetPage(tif);
		protoTrace("RECV DIS/DTC");
		emsg = "Can not continue after DIS/DTC {E107}";
		return (false);
	    case FCF_PWD:
	    case FCF_SUB:
	    case FCF_NSS:
	    case FCF_TSI:
	    case FCF_DCS:
		{
		    signalRcvd = 0;
		    if (!pageGood) recvResetPage(tif);
		    // look for high speed carrier only if training successful
		    messageReceived = !(FaxModem::recvBegin(NULL, emsg));
		    bool trainok = true;
		    short traincount = 0;
		    do {
			if (!messageReceived) messageReceived = !(recvDCSFrames(frame));
			if (recvdDCN) {
			    messageReceived = true;
			    signalRcvd = FCF_DCN;
			    lastResponse = AT_NOTHING;
			    return (false);
			}
			if (!messageReceived) {
			    trainok = recvTraining();
			    messageReceived = (!trainok && lastResponse == AT_FRH3);
			}
		    } while (!trainok && traincount++ < 3 && lastResponse != AT_FRH3 && recvFrame(frame, FCF_RCVR, timer));
		    if (messageReceived && lastResponse == AT_FRH3 && waitFor(AT_CONNECT,0)) {
			messageReceived = false;
			if (recvFrame(frame, FCF_RCVR, conf.t2Timer, true, false)) {
			    messageReceived = true;
			    signalRcvd = frame.getFCF();
			}
			if (!frame.getLength() && lastResponse == AT_NOCARRIER) {
			    // The modem may have indicated V.21 HDLC incorrectly.  TCF may be coming.  Get ready.
			    trainok = recvTraining();
			    messageReceived = (!trainok && lastResponse == AT_FRH3);
			    if (messageReceived && lastResponse == AT_FRH3 && waitFor(AT_CONNECT,0)) {
				messageReceived = false;
				if (recvFrame(frame, FCF_RCVR, conf.t2Timer, true)) {
				    messageReceived = true;
				    signalRcvd = frame.getFCF();
				}
			    }
			}
			lastResponse = AT_NOTHING;
		    } else messageReceived = false;
		    break;
		}
	    case FCF_MPS:			// MPS
	    case FCF_EOM:			// EOM
	    case FCF_EOP:			// EOP
	    case FCF_PRI_MPS:			// PRI-MPS
	    case FCF_PRI_EOM:			// PRI-EOM
	    case FCF_PRI_EOP:			// PRI-EOP
		if (!getRecvEOLCount()) {
		    // We have a null page, don't save it because it makes readers fail.
		    pageGood = false;
		    if (params.ec != EC_DISABLE) {
			if (emsg == "") {
			    /*
			     * We negotiated ECM, got no valid ECM image data, and the 
			     * ECM page reception routines did not set an error message.
			     * The empty emsg is due to the ECM routines detecting a 
			     * non-ECM-specific partial-page signal and wants it to
			     * be handled here.  The sum total of all of this, and the 
			     * fact that we got MPS/EOP/EOM tells us that the sender
			     * is not using ECM.  In an effort to salvage the session we'll
			     * disable ECM now and try to continue.
			     */
			    params.ec = EC_DISABLE;
			} else
			    return (false);
		    }
		}
		if (!pageGood && conf.badPageHandling == FaxModem::BADPAGE_RTN)
		    recvResetPage(tif);
		if (signalRcvd == 0) traceFCF("RECV recv", lastPPM);

		/*
		 * [Re]transmit post page response.
		 */
		if (pageGood || (conf.badPageHandling == FaxModem::BADPAGE_RTNSAVE && getRecvEOLCount())) {
		    if (!pageGood) lastPPM = FCF_MPS;	// FaxModem::BADPAGE_RTNSAVE
		    /*
		     * If post page message confirms the page
		     * that we just received, write it to disk.
		     */
		    if (messageReceived) {
			if (!useV34 && emsg == "") (void) switchingPause(emsg);
			/*
			 * On servers where disk access may be bottlenecked or stressed,
			 * the TIFFWriteDirectory call can lag.  The strategy, then, is
			 * to employ RNR/RR flow-control for ECM sessions and to use CRP
			 * in non-ECM sessions in order to grant TIFFWriteDirectory
			 * sufficient time to complete.
			 */
			int fcfd[2];		// flow control file descriptors for the pipe
			pid_t fcpid = -1;	// flow control process id for the child
			if (pipe(fcfd) >= 0) {
			    fcpid = fork();
			    char tbuf[1];	// trigger signal
			    tbuf[0] = 0xFF;
			    time_t rrstart = Sys::now();
			    switch (fcpid) {
				case -1:	// error
				    protoTrace("Protocol flow control unavailable due to fork error.");
				    TIFFWriteDirectory(tif);
				    Sys::close(fcfd[0]);
				    Sys::close(fcfd[1]);
				    break;
				case 0:		// child
				    Sys::close(fcfd[1]);
				    do {
					fd_set rfds;
					FD_ZERO(&rfds);
					FD_SET(fcfd[0], &rfds);
					struct timeval tv;
					tv.tv_sec = 2;		// we've got a 3-second window, use it
					tv.tv_usec = 0;
#if CONFIG_BADSELECTPROTO
					if (!select(fcfd[0]+1, (int*) &rfds, NULL, NULL, &tv)) {
#else
					if (!select(fcfd[0]+1, &rfds, NULL, NULL, &tv)) {
#endif
					    bool gotresponse = true;
					    u_short rnrcnt = 0;
					    do {
						if (emsg != "") break;
						(void) transmitFrame(params.ec != EC_DISABLE ? FCF_RNR : FCF_CRP|FCF_RCVR);
						traceFCF("RECV send", params.ec != EC_DISABLE ? FCF_RNR : FCF_CRP);
						HDLCFrame rrframe(conf.class1FrameOverhead);
						gotresponse = recvFrame(rrframe, FCF_RCVR, conf.t2Timer);
						if (gotresponse) {
						    traceFCF("RECV recv", rrframe.getFCF());
						    if (rrframe.getFCF() == FCF_DCN) {
							protoTrace("RECV recv DCN");
							emsg = "COMREC received DCN (sender abort) {E108}";
							gotEOT = true;
							recvdDCN = true;
						    } else if (params.ec != EC_DISABLE && rrframe.getFCF() != FCF_RR) {
							protoTrace("Ignoring invalid response to RNR.");
						    }
						    if (!useV34) (void) switchingPause(emsg);
						}
					    } while (!gotEOT && !recvdDCN && !gotresponse && ++rnrcnt < 2 && Sys::now()-rrstart < 60);
					    if (!gotresponse) emsg = "No response to RNR repeated 3 times. {E109}";
					} else {		// parent finished TIFFWriteDirectory
					    tbuf[0] = 0;
					}
				    } while (!gotEOT && !recvdDCN && tbuf[0] != 0 && Sys::now()-rrstart < 60);
				    Sys::read(fcfd[0], NULL, 1);
				    _exit(0);
				default:	// parent
				    Sys::close(fcfd[0]);
				    TIFFWriteDirectory(tif);
				    Sys::write(fcfd[1], tbuf, 1);
				    (void) Sys::waitpid(fcpid);
				    Sys::close(fcfd[1]);
				    break;
			    }
			} else {
			    protoTrace("Protocol flow control unavailable due to pipe error.");
			    TIFFWriteDirectory(tif);
			}
			if (emsg == "") {	// confirm only if there was no error
			    if (pageGood) {
				traceFCF("RECV send", sendERR ? FCF_ERR : FCF_MCF);
				lastMCF = Sys::now();
			    } else
				traceFCF("RECV send", FCF_RTN);

			    if (lastPPM == FCF_MPS) {
				/*
				 * Raise the HDLC transmission carrier but do not
				 * transmit MCF now.  This gives us at least a 3-second
				 * window to buffer any delays in the post-page
				 * processes.
				 */
				if (!useV34 && !atCmd(thCmd, AT_CONNECT)) {
				    emsg = "Failure to raise V.21 transmission carrier. {E101}";
				    return (false);
				}
			    } else {
				(void) transmitFrame((sendERR ? FCF_ERR : FCF_MCF)|FCF_RCVR);
				lastMCF = Sys::now();
				if (lastPPM == FCF_EOP) {
				    /*
				     * Because there are a couple of notifications that occur after this
				     * things can hang and we can miss hearing DCN.  So we do it now.
				     */
				    recvdDCN = recvEnd(NULL, emsg);
				}
			    }
			}
			/*
			 * Reset state so that the next call looks
			 * first for page carrier or frame according
			 * to what's expected.  (Grr, where's the
			 * state machine...)
			 */
			messageReceived = (lastPPM == FCF_EOM);
			ppm = modemPPMCodes[lastPPM&7];
			return (true);
		    }
		} else {
		    /*
		     * Page not received, or unacceptable; tell
		     * other side to retransmit after retrain.
		     */
		    /*
		     * As recommended in T.31 Appendix II.1, we try to
		     * prevent the rapid switching of the direction of 
		     * transmission by using +FRS.  Theoretically, "OK"
		     * is the only response, but if the sender has not
		     * gone silent, then we cannot continue anyway,
		     * and aborting here will give better information.
		     *
		     * Using +FRS is better than a software pause, which
		     * could not ensure loss of carrier.  +FRS is easier
		     * to implement than using +FRH and more reliable than
		     * using +FTS
		     */
		    if (!switchingPause(emsg)) {
			return (false);
		    }
		    signalRcvd = 0;
		    if (params.ec == EC_DISABLE && rmResponse != AT_CONNECT && !getRecvEOLCount() && (Sys::now() - lastMCF < 9)) {
			/*
			 * We last transmitted MCF a very, very short time ago, received no image data
			 * since then, and now we're seeing a PPM again.  In non-ECM mode the chances of 
			 * this meaning that we simply missed a very short page is extremely remote.  It
			 * probably means that the sender did not properly hear our MCF and that we just
			 * need to retransmit it. 
			 */
			(void) transmitFrame(FCF_MCF|FCF_RCVR);
			traceFCF("RECV send", FCF_MCF);
			lastMCF = Sys::now();
			messageReceived = (lastPPM != FCF_MPS);	// expect Phase C if MPS
		    } else {
			u_int rtnfcf = FCF_RTN;
			if (!getRecvEOLCount() || conf.badPageHandling == FaxModem::BADPAGE_DCN) {
			    /*
			     * Regardless of the BadPageHandling setting, if we get no page image data at
			     * all, then sending RTN at all risks confirming the non-page to RTN-confused 
			     * senders, which risk is far worse than just simply hanging up.
			     */
			    emsg = "PPM received with no image data.  To continue risks receipt confirmation. {E155}";
			    rtnfcf = FCF_DCN;
			}
			(void) transmitFrame(rtnfcf|FCF_RCVR);
			traceFCF("RECV send", rtnfcf);
			if (rtnfcf == FCF_DCN) {
			    recvdDCN = true;
			    return (false);
			}
			/*
			 * Reset the TIFF-related state so that subsequent
			 * writes will overwrite the previous data.
			 */
			messageReceived = true;	// expect DCS next
		    }
		}
		break;
	    case FCF_RR:
		// The sender did not hear our MCF signal.  Treat it like CRP.
	    case FCF_CRP:
		// command repeat... just repeat whatever we last sent
		if (!useV34 && !switchingPause(emsg)) return (false);
		transmitFrame(signalSent);
		traceFCF("RECV send", (u_char) signalSent[2]);
		/* fall through - to clear messageReceived and signalRcvd */
	    case FCF_MCF:
	    case FCF_CFR:
		/* It's probably just our own echo. */
		messageReceived = false;
		signalRcvd = 0;
		break;
	    case FCF_DCN:			// DCN
		protoTrace("RECV recv DCN");
		emsg = "COMREC received DCN (sender abort) {E108}";
		recvdDCN = true;
		if (prevPage && conf.saveUnconfirmedPages && getRecvEOLCount()) {	// only if there was data
		    TIFFWriteDirectory(tif);
		    protoTrace("RECV keeping unconfirmed page");
		    return (true);
		}
		return (false);
	    default:
		if (!pageGood) recvResetPage(tif);
		emsg = "COMREC invalid response received {E110}";
		return (false);
	    }
	    t2end = 0;
	} else {
	    /*
	     * We didn't get a message.  Try to be resiliant by
	     * looking for the signal again, but prevent infinite
	     * looping with a timer.  However, if the modem is on
	     * hook, then modem responds ERROR or NO CARRIER, and
	     * for those cases there is no point in resiliancy.
	     */
	    if (lastResponse == AT_NOCARRIER || lastResponse == AT_ERROR) break;
	    if (t2end) {
		if (Sys::now() > t2end)
		    break;
	    } else {
		t2end = Sys::now() + howmany(conf.t2Timer, 1000);
	    }
	}
    } while (gotCONNECT && !wasTimeout() && lastResponse != AT_EMPTYLINE);
    emsg = "V.21 signal reception timeout; expected page possibly not received in full {E111}";
    if (prevPage && conf.saveUnconfirmedPages && getRecvEOLCount()) {
	TIFFWriteDirectory(tif);
	protoTrace("RECV keeping unconfirmed page");
	return (true);
    }
    return (false);
}

void
Class1Modem::abortPageRecv()
{
    if (useV34) return;				// nothing to do in V.34
    char c = CAN;				// anything other than DC1/DC3
    putModem(&c, 1, 1);
}

bool
Class1Modem::raiseRecvCarrier(bool& dolongtrain, fxStr& emsg)
{
    if (!atCmd(conf.class1MsgRecvHackCmd, AT_OK)) {
	emsg = "Failure to receive silence (synchronization failure). {E100}";
	return (false);
    }
    /*
     * T.30 Section 5, Note 5 states that we must use long training
     * on the first high-speed data message following CTR.
     */
    fxStr rmCmd;
    if (dolongtrain) rmCmd = fxStr(curcap->value, rmCmdFmt);
    else rmCmd = fxStr(curcap[HasShortTraining(curcap)].value, rmCmdFmt);
    u_short attempts = 0;
    lastResponse = AT_NOTHING;
    do {
	(void) atCmd(rmCmd, AT_NOTHING);
	lastResponse = atResponse(rbuf, conf.class1RMPersistence ? conf.t2Timer + 2900 : conf.t2Timer - 2900);
    } while ((lastResponse == AT_NOTHING || lastResponse == AT_FCERROR) && ++attempts < conf.class1RMPersistence);
    if (lastResponse == AT_ERROR) gotEOT = true;	// on hook
    if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT,0)) {
	gotRTNC = true;
	gotEOT = false;
    }
    if (lastResponse != AT_CONNECT && !gotRTNC) {
	emsg = "Failed to properly detect high-speed data carrier. {E112}";
	return (false);
    }
    dolongtrain = false;
    return (true);
}

void
Class1Modem::abortPageECMRecv(TIFF* tif, const Class2Params& params, u_char* block, u_int fcount, u_short seq, bool pagedataseen, fxStr& emsg)
{
    if (pagedataseen) {
	writeECMData(tif, block, (fcount * frameSize), params, (seq |= 2), emsg);
	if (conf.saveUnconfirmedPages) {
	    protoTrace("RECV keeping unconfirmed page");
	    prevPage++;
	}
    }
    free(block);
}

/*
 * Receive Phase C data in T.30-A ECM mode.
 */
bool
Class1Modem::recvPageECMData(TIFF* tif, const Class2Params& params, fxStr& emsg)
{
    HDLCFrame frame(5);					// A+C+FCF+FCS=5 bytes
    u_char* block = (u_char*) malloc(frameSize*256);	// 256 frames per block - totalling 16/64KB
    fxAssert(block != NULL, "ECM procedure error (receive block).");
    memset(block, 0, (size_t) frameSize*256);
    bool lastblock = false;
    bool pagedataseen = false;
    u_short seq = 1;					// sequence code for the first block
    prevBlock = 0;
    u_int lastSignalRcvd = 0;

    do {
	u_int fnum = 0;
	char ppr[32];					// 256 bits
	for (u_int i = 0; i < 32; i++) ppr[i] = 0xff;	// ppr defaults to all 1's, T.4 A.4.4
	u_short rcpcnt = 0;
	u_short pprcnt = 0;
	u_int fcount = 0;
	u_short syncattempts = 0;
	bool blockgood = false, dolongtrain = false;
	bool gotoPhaseD = false;
	do {
	    sendERR = false;
	    resetBlock();
	    lastSignalRcvd = signalRcvd;
	    signalRcvd = 0;
	    rcpcnt = 0;
	    bool dataseen = false;
	    bool retryrmcmd;
	    int rmattempted = 0;
	    do {
		retryrmcmd = false;
		if (!useV34 && !gotoPhaseD) {
		    gotRTNC = false;
		    if (!raiseRecvCarrier(dolongtrain, emsg) && !gotRTNC) {
			if (wasTimeout()) {
			    abortReceive();		// return to command mode
			    setTimeout(false);
			}
			long wait = BIT(curcap->br) & BR_ALL ? 273066 / (curcap->br+1) : conf.t2Timer;
			if (lastResponse != AT_NOCARRIER && atCmd(rhCmd, AT_CONNECT, wait)) {	// wait longer
			    // sender is transmitting V.21 instead, we may have
			    // missed the first signal attempt, but should catch
			    // the next attempt.  This "simulates" adaptive receive.
			    emsg = "";	// reset
			    gotRTNC = true;
			} else {
			    if (wasTimeout()) abortReceive();
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    return (false);
			}
		    }
		}
		if (useV34 || gotRTNC) {		// V.34 mode or if +FRH:3 in adaptive reception
		    if (!gotEOT) {
			bool gotprimary = false;
			if (useV34) gotprimary = waitForDCEChannel(false);
			while (!sendERR && !gotEOT && (gotRTNC || (ctrlFrameRcvd != fxStr::null))) {
			    /*
			     * Remote requested control channel retrain, the remote didn't
			     * properly hear our last signal, and/or we got an EOR signal 
			     * after PPR.  So now we have to use a signal from the remote
			     * and then respond appropriately to get us back or stay in sync.
			     * DCS::CFR - PPS::PPR/MCF - EOR::ERR
			     */
			    HDLCFrame rtncframe(conf.class1FrameOverhead);
			    bool gotrtncframe = false;
			    if (useV34) {
				if (ctrlFrameRcvd != fxStr::null) {
				    gotrtncframe = true;
				    for (u_int i = 0; i < ctrlFrameRcvd.length(); i++)
					rtncframe.put(frameRev[ctrlFrameRcvd[i] & 0xFF]);
				    traceHDLCFrame("-->", rtncframe);
				} else
				    gotrtncframe = recvFrame(rtncframe, FCF_RCVR, conf.t2Timer);
			    } else {
				gotrtncframe = recvFrame(rtncframe, FCF_RCVR, conf.t2Timer, true, false);
			    }
			    if (gotrtncframe) {
				traceFCF("RECV recv", rtncframe.getFCF());
				switch (rtncframe.getFCF()) {
				    case FCF_PPS:
					if (rtncframe.getLength() > 5) {
					    u_int fc = frameRev[rtncframe[6]] + 1;
					    if ((fc == 256 || fc == 1) && !dataseen) fc = 0;	// distinguish 0 from 1 and 256
					    traceFCF("RECV recv", rtncframe.getFCF2());
					    u_int pgcount = u_int(prevPage/256)*256+frameRev[rtncframe[4]];	// cope with greater than 256 pages
					    protoTrace("RECV received %u frames of block %u of page %u", \
						fc, frameRev[rtncframe[5]]+1, pgcount+1);
					    switch (rtncframe.getFCF2()) {
						case 0: 	// PPS-NULL
						case FCF_EOM:
						case FCF_MPS:
						case FCF_EOP:
						case FCF_PRI_EOM:
						case FCF_PRI_MPS:
						case FCF_PRI_EOP:
						    if (!useV34 && !switchingPause(emsg)) {
							abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
							return (false);
						    }
						    if (pgcount > prevPage || (pgcount == prevPage && frameRev[rtncframe[5]] >= prevBlock)) {
							(void) transmitFrame(FCF_PPR, fxStr(ppr, 32));
							traceFCF("RECV send", FCF_PPR);
						    } else {
							(void) transmitFrame(FCF_MCF|FCF_RCVR);
							traceFCF("RECV send", FCF_MCF);
						    }
						    break;
					    }
					}
					break;
				    case FCF_EOR:
					if (rtncframe.getLength() > 5) {
					    traceFCF("RECV recv", rtncframe.getFCF2());
					    switch (rtncframe.getFCF2()) {
						case 0: 	// PPS-NULL
						case FCF_EOM:
						case FCF_MPS:
						case FCF_EOP:
						case FCF_PRI_EOM:
						case FCF_PRI_MPS:
						case FCF_PRI_EOP:
						    if (fcount) {
							/*
							 * The block hasn't been written to disk.
							 * This is expected when the sender sends
							 * EOR after our PPR (e.g. after the 4th).
							 */
							blockgood = true;
							signalRcvd = rtncframe.getFCF2();
							if (signalRcvd) lastblock = true;
							sendERR = true;
						    } else {
							if (!useV34 && !switchingPause(emsg)) {
							    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
							    return (false);
							}
							(void) transmitFrame(FCF_ERR|FCF_RCVR);
							traceFCF("RECV send", FCF_ERR);
						    }
						    break;
					    }
					}
					break;
				    case FCF_CTC:
					{
					    u_int dcs;			// possible bits 1-16 of DCS in FIF
					    if (useV34) {
						// T.30 F.3.4.5 Note 1 does not permit CTC in V.34-fax
						emsg = "Received invalid CTC signal in V.34-Fax. {E113}";
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					    }
					    /*
					     * See the other comments about T.30 A.1.3.  Some senders
					     * are habitually wrong in sending CTC at incorrect moments.
					     */
					    // use 16-bit FIF to alter speed, curcap
					    dcs = rtncframe[3] | (rtncframe[4]<<8);
					    curcap = findSRCapability(dcs&DCS_SIGRATE, recvCaps);
					    processNewCapabilityUsage();
					    // requisite pause before sending response (CTR)
					    if (!switchingPause(emsg)) {
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					    }
					    (void) transmitFrame(FCF_CTR|FCF_RCVR);
					    traceFCF("RECV send", FCF_CTR);
					    dolongtrain = true;
					    pprcnt = 0;
					    break;
					}
				    case FCF_CRP:
					// command repeat... just repeat whatever we last sent
					if (!useV34 && !switchingPause(emsg)) {
					    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
					    return (false);
					}
					transmitFrame(signalSent);
					traceFCF("RECV send", (u_char) signalSent[2]);
					break;
				    case FCF_DCN:
					emsg = "COMREC received DCN (sender abort) {E108}";
					gotEOT = true;
					recvdDCN = true;
					continue;
				    case FCF_MCF:
				    case FCF_CFR:
				    case FCF_CTR:
					if ((rtncframe[2] & 0x80) == FCF_RCVR) {
					    /*
					     * Echo on the channel may be so lagged that we're hearing 
					     * ourselves.  Ignore it.  Try again.
					     */
					    break;
					}
					/* intentional pass-through */
				    default:
					// The message is not ECM-specific: fall out of ECM receive, and let
					// the earlier message-handling routines try to cope with the signal.
					signalRcvd = rtncframe.getFCF();
					messageReceived = true;
					abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
					if (getRecvEOLCount() == 0) {
					    prevPage--;		// counteract the forthcoming increment
					    return (true);
					} else {
					    emsg = "COMREC invalid response received {E110}";	// plain ol' error
					    return (false);
					}
				}
				if (!sendERR) {	// as long as we're not trying to send the ERR signal (set above)
				    if (useV34) gotprimary = waitForDCEChannel(false);
				    else {
					gotRTNC = false;
					if (!raiseRecvCarrier(dolongtrain, emsg) && !gotRTNC) {
					    if (wasTimeout()) {
						abortReceive();	// return to command mode
						setTimeout(false);
					    }
					    long wait = BIT(curcap->br) & BR_ALL ? 273066 / (curcap->br+1) : conf.t2Timer;
					    if (lastResponse != AT_NOCARRIER && atCmd(rhCmd, AT_CONNECT, wait)) {	// wait longer
						// simulate adaptive receive
						emsg = "";		// clear the failure
						gotRTNC = true;
					    } else {
						if (wasTimeout()) abortReceive();
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					    }
					} else gotprimary = true;
				    }
				}
			    } else {
				gotprimary = false;
				if (!useV34) {
				    if (wasTimeout()) {
					abortReceive();
					break;
				    }
				    if (lastResponse == AT_NOCARRIER) {
					/*
					 * The modem reported V.21 HDLC but then reported that carrier was lost.
					 * The modem may have erred in its detection of the high-speed carrier.
					 * But it may also have detected echo from our end or there may be 
					 * another receiver on the line (the sender is sharing the line with two
					 * machines).  Try AT+FRM=n again.
					 */
					retryrmcmd = true;
					if (rmattempted+1 < 2) {
					    gotRTNC = false;
					} else {
					    gotRTNC = true;
					    if (!atCmd(rhCmd, AT_CONNECT, conf.t1Timer)) break;
					}
				    } else {
					if (!atCmd(rhCmd, AT_CONNECT, conf.t1Timer)) break;
				    }
				}
			    }
			}
			if (!gotprimary && !sendERR && !(retryrmcmd && rmattempted+1 < 2)) {
			    if (emsg == "") {
				if (useV34) emsg = "Failed to properly open V.34 primary channel. {E114}";
				else emsg = "Failed to properly detect high-speed data carrier. {E112}";
			    }
			    protoTrace(emsg);
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    return (false);
			}
		    }
		    if (gotEOT) {		// intentionally not an else of the previous if
			if (useV34 && emsg == "") emsg = "Received premature V.34 termination. {E115}";
			protoTrace(emsg);
			abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			return (false);
		    }
		}
	    } while (retryrmcmd && ++rmattempted < 2);
	    /*
	     * Buffering and flow control must be done after AT+FRM=n.
	     * We do not flush in order avoid losing data already buffered.
	     */
	    setInputBuffering(true);
	    if (flowControl == FLOW_XONXOFF)
		(void) setXONXOFF(FLOW_NONE, FLOW_XONXOFF, ACT_NOW);
	    gotoPhaseD = false;
	    if (!sendERR && (useV34 || syncECMFrame())) {	// no synchronization needed w/V.34-fax
		time_t start = Sys::now();
		do {
		    frame.reset();
		    if (recvECMFrame(frame)) {
			if (frame[2] == 0x60) {		// FCF is FCD
			    dataseen = true;
			    pagedataseen = true;
			    rcpcnt = 0;			// reset RCP counter
			    fnum = frameRev[frame[3]];	// T.4 A.3.6.1 says LSB2MSB
			    protoTrace("RECV received frame number %u", fnum);
			    // some modems may erroneously recreate valid CRC on short frames, so possibly check length, too
			    if (conf.class1ECMCheckFrameLength ? frame.checkCRC() && frame.getLength() == frameSize+6 : frame.checkCRC()) {
				// store received frame in block at position fnum (A+C+FCF+Frame No.=4 bytes)
				for (u_int i = 0; i < frameSize; i++) {
				    if (frame.getLength() - 6 > i)	// (A+C+FCF+Frame No.+FCS=6 bytes)
					block[fnum*frameSize+i] = frameRev[frame[i+4]];	// LSB2MSB
				}
				if (fcount < (fnum + 1)) fcount = fnum + 1;
				// valid frame, set the corresponding bit in ppr to 0
				u_int pprpos, pprval;
				for (pprpos = 0, pprval = fnum; pprval >= 8; pprval -= 8) pprpos++;
				if (ppr[pprpos] & frameRev[1 << pprval]) ppr[pprpos] ^= frameRev[1 << pprval];
			    } else {
				protoTrace("RECV frame FCS check failed");
			    }
			} else if (frame[2] == 0x61 && frame.checkCRC()) {	// FCF is RCP
			    rcpcnt++;
			} else {
			    dataseen = true;
			    if (frame.getLength() > 4 && frame.checkCRC()) {
				traceFCF("Invalid and confusing placement of", frame.getFCF());
			    } else {
				protoTrace("HDLC frame with bad FCF %#x", frame[2]);
			    }
			}
		    } else {
			dataseen = true;	// assume that garbage was meant to be data
			if (!useV34) syncECMFrame();
			if (useV34 && (gotEOT || gotCTRL)) rcpcnt = 3;
		    }
		    // some senders don't send the requisite three RCP signals
		} while (rcpcnt == 0 && (unsigned) Sys::now()-start < 5*60);	// can't expect 50 ms of flags, some violate T.4 A.3.8
		if (flowControl == FLOW_XONXOFF)
		    (void) setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_DRAIN);
		setInputBuffering(false);
		if (useV34) {
		    if (!gotEOT && !gotCTRL && !waitForDCEChannel(true)) {
			emsg = "Failed to properly open V.34 control channel. {E116}";
			protoTrace(emsg);
			abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			return (false);
		    }
		    if (gotEOT) {
			emsg = "Received premature V.34 termination. {E115}";
			protoTrace(emsg);
			abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			return (false);
		    }
		} else {
		    if (!endECMBlock()) {				// wait for <DLE><ETX>
			if (wasTimeout()) {
			    abortReceive();
			    emsg = "Timeout waiting for Phase C carrier drop. {E154}";
			    protoTrace(emsg);
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    return (false);
			}
		    }
		}
		if (!useV34) {
		    // wait for message carrier to drop
		    time_t nocarrierstart = Sys::now();
		    bool gotnocarrier = false;
		    do {
			gotnocarrier = waitFor(AT_NOCARRIER, 2*1000);
		    } while (!gotnocarrier && lastResponse != AT_EMPTYLINE && Sys::now() < (nocarrierstart + 5));
		}
		bool gotpps = false;
		HDLCFrame ppsframe(conf.class1FrameOverhead);
		u_short recvFrameCount = 0;
		do {
		    /*
		     * It is possible that the high-speed carrier drop was
		     * detected in error or that some line noise caused it but
		     * that the sender has not disconnected.  It is possible
		     * then, that T2 will not be long enough to receive the
		     * partial-page signal because the sender is still transmitting
		     * high-speed data.  So we calculate the wait for 80KB (the 
		     * ECM block plus some wriggle-room) at the current bitrate.
		     */
		    u_int br = useV34 ? primaryV34Rate : curcap->br + 1;
		    long wait = br >= 1 && br <= 15 ? 273066 / br : conf.t2Timer;
		    gotpps = recvFrame(ppsframe, FCF_RCVR, wait);	// wait longer
		} while (!gotpps && gotCONNECT && !wasTimeout() && !gotEOT && ++recvFrameCount < 5);
		if (gotpps) {
		    traceFCF("RECV recv", ppsframe.getFCF());
		    if (ppsframe.getFCF() == FCF_PPS) {
			// sender may violate T.30-A.4.3 and send another signal (i.e. DCN)
			traceFCF("RECV recv", ppsframe.getFCF2());
		    }
		    u_int pgcount = u_int(prevPage/256)*256+frameRev[ppsframe[4]];	// cope with greater than 256 pages
		    switch (ppsframe.getFCF()) {
			/*
			 * PPS is the only valid signal, Figure A.8/T.30; however, some
			 * senders don't handle T.30 A.1.3 ("When PPR is received four
			 * times for the same block...") properly (possibly because T.30
			 * A.4.1 isn't clear about the "per-block" requirement), and so
			 * it is possible for us to get CTC or EOR here (if the modem
			 * quickly reported NO CARRIER when we went looking for the
			 * non-existent high-speed carrier and the sender is persistent).
			 *
			 * CRP is a bizarre signal to get instead of PPS, but some
			 * senders repeatedly transmit this instead of PPS.  So to
			 * handle it as best as possible we interpret the signal as
			 * meaning PPS-NULL (full block) unless there was no data seen
			 * (in which case PPS-MPS is assumed) in order to prevent 
			 * any data loss, and we let the sender cope with it from there.
			 *
			 * Because there is no way to express "zero" in the frame count
			 * byte there exists some confusion in some senders which attempt
			 * to do just that.  Consequently, the frame count values of 0x00
			 * and 0xFF need consideration as to whether they represent 0, 1, 
			 * or 256.  To allow for the bizarre situation where a sender may
			 * signal PPS-NULL with a frame count less than 256 we trust the
			 * PPS-NULL frame count except in cases where it is determined to
			 * be "1" because most-likely that determination only comes from
			 * some garbage detected during the high-speed carrier.
			 */
			case FCF_PPS:
			case FCF_CRP:
			    {
				u_int fc = ppsframe.getFCF() == FCF_CRP ? 256 : frameRev[ppsframe[6]] + 1;
				if ((fc == 256 || fc == 1) && !dataseen) fc = 0;	// distinguish 0 from 1 and 256
				// See comment above.  It's extremely unlikely to get PPS-NULL with a frame-count meaning "1"...
				if (ppsframe.getFCF() == FCF_PPS && ppsframe.getFCF2() == 0x00 && fc == 1) fc = 0;
				if (fcount < fc) fcount = fc;
				if (ppsframe.getFCF() == FCF_CRP) {
				    if (fc) ppsframe[3] = 0x00;		// FCF2 = NULL
				    else ppsframe[3] = FCF_MPS;
				    protoTrace("RECV unexpected CRP - assume %u frames of block %u of page %u", \
					fc, prevBlock + 1, prevPage + 1);
				} else {
				    protoTrace("RECV received %u frames of block %u of page %u", \
					fc, frameRev[ppsframe[5]]+1, pgcount+1);
				}
				blockgood = true;
				/*
				 * The sender may send no frames.  This will happen for at least three
				 * reasons.
				 *
				 * 1) If we previously received data from this block and responded
				 * with PPR but the sender is done retransmitting frames as the sender
				 * thinks that our PPR signal did not indicate any frame that the 
				 * sender transmitted.  This should only happen with the last frame
				 * of a block due to counting errors.  So, in the case where we received
				 * no frames from the sender we ignore the last frame in the block when
				 * checking.
				 *
				 * 2) The sender feeds paper into a scanner during the initial
				 * synchronization and it expected another page but didn't get it 
				 * (e.g. paper feed problem).  We respond with a full PPR in hopes that
				 * the sender knows what they're doing by sending PPS instead of DCN.
				 * The sender can recover by sending data with the block retransmission.
				 *
				 * 3) The sender sent only one frame but for some reason we did not see
				 * any data, and so the frame-count in the PPS signal ended up getting
				 * interpreted as a zero.  Only in the case that the frame happens to be
				 * the last frame in a block and we're dealing with MH, MR, or MMR data 
				 * we will send MCF (to accomodate #1), and so this frame will then be 
				 * lost.  This should be rare and have little impact on actual image data
				 * loss when it does occur.  This approach cannot be followed with JPEG
				 * and JBIG data formats or when the signal is PPS-NULL.  This approach
				 * cannot be followed when we previously saw exactly one frame of data.
				 */
				if (fcount) {
				    u_int fbad = 0;
				    for (u_int i = 0; i <= (fcount - ((fcount < 2 || fc || params.df > DF_2DMMR || ppsframe.getFCF() == 0) ? 1 : 2)); i++) {
					u_int pprpos, pprval;
					for (pprpos = 0, pprval = i; pprval >= 8; pprval -= 8) pprpos++;
					if (ppr[pprpos] & frameRev[1 << pprval]) {
					    blockgood = false;
					    fbad++;
					}
				    }
				    dataSent += fcount;
				    dataMissed += fbad;
				    if (fcount && ! blockgood) protoTrace("Block incomplete: %d frames (%d%%) corrupt or missing", fbad, ((fbad*100)/fcount));
				    if (pgcount < prevPage || (pgcount == prevPage && frameRev[ppsframe[5]] < prevBlock))
					blockgood = false;	// we already confirmed this block receipt... (see below)
				} else {
				    blockgood = false;	// MCF only if we have data
				}

				// requisite pause before sending response (PPR/MCF)
				if (!blockgood && !useV34 && !switchingPause(emsg)) {
				    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
				    return (false);
				}
			    }
			    /* ... pass through ... */
			case FCF_CTC:
			case FCF_EOR:
			    if (! blockgood) {
				if ((ppsframe.getFCF() == FCF_CTC || ppsframe.getFCF() == FCF_EOR) &&
				     (!useV34 || !conf.class1PersistentECM)) {	// only if we can make use of the signal
				    signalRcvd = ppsframe.getFCF();
				    pprcnt = 4;
				}
				if (signalRcvd == 0) {
				    if (pgcount > prevPage || (pgcount == prevPage && frameRev[ppsframe[5]] >= prevBlock)) {
					// inform the remote that one or more frames were invalid
					transmitFrame(FCF_PPR, fxStr(ppr, 32));
					traceFCF("RECV send", FCF_PPR);
					pprcnt++;
					if (pprcnt > 4) pprcnt = 4;		// could've been 4 before increment
				    } else {
					/*
					 * The silly sender already sent us this block and we already confirmed it.
					 * Just confirm it again, but let's behave as if we sent a full PPR without
					 * incrementing pprcnt.
					 */
					(void) transmitFrame(FCF_MCF|FCF_RCVR);
					traceFCF("RECV send", FCF_MCF);
					for (u_int i = 0; i < 32; i++) ppr[i] = 0xff;	// ppr defaults to all 1's, T.4 A.4.4
				    }
				}
				if (pprcnt == 4 && (!useV34 || !conf.class1PersistentECM)) {
				    HDLCFrame rtnframe(conf.class1FrameOverhead);
				    if (signalRcvd == 0) {
					// expect sender to send CTC/EOR after every fourth PPR, not just the fourth
					protoTrace("RECV sent fourth PPR");
				    } else {
					// we already got the signal
					rtnframe.put(ppsframe, ppsframe.getLength());
				    }
				    pprcnt = 0;
				    if (signalRcvd != 0 || recvFrame(rtnframe, FCF_RCVR, conf.t2Timer)) {
					bool gotrtnframe = true;
					if (signalRcvd == 0) traceFCF("RECV recv", rtnframe.getFCF());
					else signalRcvd = 0;		// reset it, we're in-sync now
					recvFrameCount = 0;
					lastResponse = AT_NOTHING;
					while (rtnframe.getFCF() == FCF_PPS && !gotEOT && recvFrameCount < 5 && gotrtnframe) {
					    // we sent PPR, but got PPS again...
					    if (!useV34 && !switchingPause(emsg)) {
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					    }
					    transmitFrame(FCF_PPR, fxStr(ppr, 32));
					    traceFCF("RECV send", FCF_PPR);
					    gotrtnframe = recvFrame(rtnframe, FCF_RCVR, conf.t2Timer);
					    if (gotrtnframe)
						traceFCF("RECV recv", rtnframe.getFCF());
					    recvFrameCount++;
					}
					u_int dcs;			// possible bits 1-16 of DCS in FIF
					switch (rtnframe.getFCF()) {
					    case FCF_CTC:
						if (useV34) {
						    // T.30 F.3.4.5 Note 1 does not permit CTC in V.34-fax
						    emsg = "Received invalid CTC signal in V.34-Fax. {E113}";
						    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						    return (false);
						}
						// use 16-bit FIF to alter speed, curcap
						dcs = rtnframe[3] | (rtnframe[4]<<8);
						curcap = findSRCapability(dcs&DCS_SIGRATE, recvCaps);
						processNewCapabilityUsage();
						// requisite pause before sending response (CTR)
						if (!switchingPause(emsg)) {
						    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						    return (false);
						}
						(void) transmitFrame(FCF_CTR|FCF_RCVR);
						traceFCF("RECV send", FCF_CTR);
						dolongtrain = true;
						break;
					    case FCF_EOR:
						traceFCF("RECV recv", rtnframe.getFCF2());
						/*
						 * It may be wise to disconnect here if MMR is being
						 * used because there will surely be image data loss.
						 * However, since the sender knows what the extent of
						 * the data loss will be, we'll naively assume that
						 * the sender knows what it's doing, and we'll
						 * proceed as instructed by it.
						 */
						blockgood = true;
						switch (rtnframe.getFCF2()) {
						    case 0:
							// EOR-NULL partial page boundary
							break;
						    case FCF_EOM:
						    case FCF_MPS:
						    case FCF_EOP:
						    case FCF_PRI_EOM:
						    case FCF_PRI_MPS:
						    case FCF_PRI_EOP:
							lastblock = true;
							signalRcvd = rtnframe.getFCF2();
							break;
						    default:
							emsg = "COMREC invalid response to repeated PPR received {E117}";
							abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
							return (false);
						}
						sendERR = true;		// do it later
						break;
					    case FCF_DCN:
						emsg = "COMREC received DCN (sender abort) {E108}";
						gotEOT = true;
						recvdDCN = true;  
					    default:
						if (emsg == "") emsg = "COMREC invalid response to repeated PPR received {E117}";
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					}
				    } else {
					emsg = "T.30 T2 timeout, expected signal not received {E118}";
					abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
					return (false);
				    }
				}
			    }
			    if (signalRcvd == 0) {		// don't overwrite EOR settings
				switch (ppsframe.getFCF2()) {
				    case 0:
					// PPS-NULL partial page boundary
					break;
				    case FCF_EOM:
				    case FCF_MPS:
				    case FCF_EOP:
				    case FCF_PRI_EOM:
				    case FCF_PRI_MPS:
				    case FCF_PRI_EOP:
					lastblock = true;
					signalRcvd = ppsframe.getFCF2();
					break;
				    default:
					if (blockgood) {
					    emsg = "COMREC invalid partial-page signal received {E119}";
					    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
					    return (false);
					}
					/*
					 * If the sender signalled PPS-<??> (where the FCF2 is meaningless) 
					 * and if the block isn't good then we already signalled back PPR... 
					 * which is appropriate despite whatever the strange FCF2 was supposed
					 * to mean, and hopefully it will not re-use it on the next go-around.
					 */
					break;
				}
			    }
			    break;
			case FCF_DCN:
			    emsg = "COMREC received DCN (sender abort) {E108}";
			    gotEOT = true;
			    recvdDCN = true;
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    return (false);
			default:
			    // The message is not ECM-specific: fall out of ECM receive, and let
			    // the earlier message-handling routines try to cope with the signal.
			    signalRcvd = ppsframe.getFCF();
			    messageReceived = true;
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    if (getRecvEOLCount() == 0) {
				 prevPage--;		// counteract the forthcoming increment
				return (true);
			    } else {
				emsg = "COMREC invalid response received {E110}";	// plain ol' error
				return (false);
			    }
		    }
		} else {
		    emsg = "T.30 T2 timeout, expected signal not received {E118}";
		    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
		    return (false);
		}
	    } else {
		if (wasTimeout()) {
		    abortReceive();
		    if (!useV34) {
			// must now await V.21 signalling
			long wait = BIT(curcap->br) & BR_ALL ? 273066 / (curcap->br+1) : conf.t2Timer;
			gotRTNC = atCmd(rhCmd, AT_CONNECT, wait);
			gotoPhaseD = gotRTNC;
			if (!gotRTNC) syncattempts = 21;
		    }
		}
		if (syncattempts++ > 20) {
		    emsg = "Cannot synchronize ECM frame reception. {E120}";
		    abortPageECMRecv(tif, params, block, fcount, seq, true, emsg);
		    return(false);
		}
	    }
	} while (! blockgood);

	u_int cc = fcount * frameSize;
	if (lastblock) {
	    // trim zero padding
	    while (cc > 0 && block[cc - 1] == 0) cc--;
	}
	// write the block to file
	if (lastblock) seq |= 2;			// seq code for the last block

	/*
	 * On servers where disk or CPU may be bottlenecked or stressed,
	 * the writeECMData call can lag.  The strategy, then, is
	 * to employ RNR/RR flow-control in order to grant writeECMData
	 * sufficient time to complete.   
	*/
	int fcfd[2];		// flow control file descriptors for the pipe
	pid_t fcpid = -1;	// flow control process id for the child
	if (pipe(fcfd) >= 0) {
	    fcpid = fork();
	    char tbuf[1];	// trigger signal
	    tbuf[0] = 0xFF;
	    time_t rrstart = Sys::now();
	    switch (fcpid) {
		case -1:	// error
		    protoTrace("Protocol flow control unavailable due to fork error.");
		    writeECMData(tif, block, cc, params, seq, emsg);
		    Sys::close(fcfd[0]);
		    Sys::close(fcfd[1]);
		    break;
		case 0:		// child
		    Sys::close(fcfd[1]);
		    do {
			fd_set rfds;
			FD_ZERO(&rfds);
			FD_SET(fcfd[0], &rfds);
			struct timeval tv;
			tv.tv_sec = 1;		// 1000 ms should be safe
			tv.tv_usec = 0;
#if CONFIG_BADSELECTPROTO
			if (!select(fcfd[0]+1, (int*) &rfds, NULL, NULL, &tv)) {
#else
			if (!select(fcfd[0]+1, &rfds, NULL, NULL, &tv)) {
#endif
			    bool gotresponse = true;
			    u_short rnrcnt = 0;
			    do {
				if (!useV34) (void) switchingPause(emsg);
				if (emsg != "") break;
				(void) transmitFrame(FCF_RNR|FCF_RCVR);
				traceFCF("RECV send", FCF_RNR);
				HDLCFrame rrframe(conf.class1FrameOverhead);
				gotresponse = recvFrame(rrframe, FCF_RCVR, conf.t2Timer);
				if (gotresponse) {
				    traceFCF("RECV recv", rrframe.getFCF());
				    if (rrframe.getFCF() == FCF_DCN) {
					protoTrace("RECV recv DCN");
					emsg = "COMREC received DCN (sender abort) {E108}";
					gotEOT = true;
					recvdDCN = true;
				    } else if (params.ec != EC_DISABLE && rrframe.getFCF() != FCF_RR) {
					protoTrace("Ignoring invalid response to RNR.");
				    }
				}
			    } while (!recvdDCN && !gotEOT && !gotresponse && ++rnrcnt < 2 && Sys::now()-rrstart < 60);
			    if (!gotresponse) emsg = "No response to RNR repeated 3 times. {E109}";
			} else tbuf[0] = 0;	// parent finished writeECMData
		    } while (!gotEOT && !recvdDCN && tbuf[0] != 0 && Sys::now()-rrstart < 60);
		    Sys::read(fcfd[0], NULL, 1);
		    _exit(0);
		default:	// parent
		    Sys::close(fcfd[0]);
		    writeECMData(tif, block, cc, params, seq, emsg);
		    Sys::write(fcfd[1], tbuf, 1);
		    (void) Sys::waitpid(fcpid);
		    Sys::close(fcfd[1]);
		    break;
	    }
	} else {
	    protoTrace("Protocol flow control unavailable due to pipe error.");
	    writeECMData(tif, block, cc, params, seq, emsg);
	}
	seq = 0;					// seq code for in-between blocks

	if (!lastblock) {
	    // confirm block received as good
	    if (!useV34) (void) switchingPause(emsg);
	    (void) transmitFrame((sendERR ? FCF_ERR : FCF_MCF)|FCF_RCVR);
	    traceFCF("RECV send", sendERR ? FCF_ERR : FCF_MCF);
	}
	prevBlock++;
    } while (! lastblock);

    free(block);
    recvEndPage(tif, params);

    if (getRecvEOLCount() == 0) {
	// Just because image data blocks are received properly doesn't guarantee that
	// those blocks actually contain image data.  If the decoder finds no image
	// data at all we send DCN instead of MCF in hopes of a retransmission.
	emsg = "ECM page received containing no image data. {E121}";
	return (false);
    }
    if (!signalRcvd) {
	// It appears that the sender did something bizarre such as first signaling
	// PPS-MPS and then after a PPR exchange later signaled PPS-NULL.  We've come
	// to here because lastblock was set true as was blockgood.  So let's restore
	// signalRcvd to the non-null value so that we can do something useful after.
	signalRcvd = lastSignalRcvd;
    }
    return (true);   		// signalRcvd is set, full page is received...
}

/*
 * Receive Phase C data w/ or w/o copy quality checking.
 */
bool
Class1Modem::recvPageData(TIFF* tif, fxStr& emsg)
{
    bool ret = false;
    /*
     * T.30-A ECM mode requires a substantially different protocol than non-ECM faxes.
     */
    if (params.ec != EC_DISABLE) {
	if (!recvPageECMData(tif, params, emsg)) {
	    /*
	     * The previous page experienced some kind of error.  Falsify
	     * some event settings in order to cope with the error gracefully.
	     */
	    signalRcvd = FCF_EOP;
	    messageReceived = true;
	    if (prevPage)
		recvEndPage(tif, params);
	}
	ret = true;		// no RTN with ECM
    } else {
	(void) recvPageDLEData(tif, checkQuality(), params, emsg);
	dataSent += getRecvEOLCount();
	dataMissed += getRecvBadLineCount();
	TIFFSetField(tif, TIFFTAG_CLEANFAXDATA, getRecvBadLineCount() ?
	    CLEANFAXDATA_REGENERATED : CLEANFAXDATA_CLEAN);
	if (getRecvBadLineCount()) {
	    TIFFSetField(tif, TIFFTAG_BADFAXLINES, getRecvBadLineCount());
	    TIFFSetField(tif, TIFFTAG_CONSECUTIVEBADFAXLINES,
		getRecvConsecutiveBadLineCount());
	}
	ret = isQualityOK(params);
    }
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, getRecvEOLCount());
    return (ret);
}

/*
 * Complete a receive session.
 */
bool
Class1Modem::recvEnd(FaxSetup* setupinfo, fxStr& emsg)
{
    if (setupinfo) {
	/*
	 * Update FaxMachine info...
	 */
	setupinfo->senderSkipsV29 = senderSkipsV29;
	setupinfo->senderHasV17Trouble = senderHasV17Trouble;
	setupinfo->senderDataSent = dataSent;
	setupinfo->senderDataMissed = dataMissed;
    }
    if (!recvdDCN && !gotEOT) {
	u_int t1 = howmany(conf.t1Timer, 1000);	// T1 timer in seconds
	time_t start = Sys::now();
	/*
	 * Wait for DCN and retransmit ack of EOP if needed.
	 */
	HDLCFrame frame(conf.class1FrameOverhead);
	do {
	    gotRTNC = false;
	    if (recvFrame(frame, FCF_RCVR, conf.t2Timer)) {
		traceFCF("RECV recv", frame.getFCF());
		switch (frame.getFCF()) {
		case FCF_PPS:
		case FCF_EOP:
		case FCF_CRP:
		    if (!useV34) (void) switchingPause(emsg);
		    (void) transmitFrame(FCF_MCF|FCF_RCVR);
		    traceFCF("RECV send", FCF_MCF);
		    break;
		case FCF_DCN:
		    recvdDCN = true;
		    break;
		default:
		    if (!useV34) (void) switchingPause(emsg);
		    transmitFrame(FCF_DCN|FCF_RCVR);
		    recvdDCN = true;
		    break;
		}
	    } else if (gotRTNC) {
		(void) transmitFrame(FCF_MCF|FCF_RCVR);
		traceFCF("RECV send", FCF_MCF);
	    } else if (!wasTimeout() && lastResponse != AT_FCERROR && lastResponse != AT_FRH3) {
		/*
		 * Beware of unexpected responses from the modem.  If
		 * we lose carrier, then we can loop here if we accept
		 * null responses, or the like.
		 */
		break;
	    }
	} while ((unsigned) Sys::now()-start < t1 && (!frame.isOK() || !recvdDCN));
    }
    setInputBuffering(true);
    return (true);
}

/*
 * Abort an active receive session.
 */
void
Class1Modem::recvAbort()
{
    if (!recvdDCN && !gotEOT) {
	fxStr emsg;
	if (!useV34) switchingPause(emsg);
	transmitFrame(FCF_DCN|FCF_RCVR);
    }
    recvdDCN = true;				// don't hang around in recvEnd
}