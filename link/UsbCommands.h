#pragma once
// USB command parser and status output (!status, !report, !i2c-probe, ...). Part of link.ino
// (included there just before loop()): it uses the sketch's globals and helpers.
void serviceUsb() {
  StageScope stage(ST_USB_SERVICE);
  static char input[32]; static unsigned used = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      input[used] = 0; used = 0;
      if (!strcmp(input,"!report") && SENSOR) {
        forceReport = true; reportDue = true;
      } else if (!strcmp(input,"!standby-test") && SENSOR) {
        logLine(deepSleep ? "# deep standby is active" : "# light sleep: a USB host was seen this session; unplug USB and reset to use deep standby");
      } else if (!strcmp(input,"!uart-test") && SENSOR) {
        // Simulate a missing TX-complete indication on the next wake command.
        radioUart.injectCompletionTimeout = true;
        forceReport = true; reportDue = true;
        logLine("# injecting one UART completion timeout; automatic retry expected in 5 s");
      } else if (!strcmp(input,"!xosc-test") || (!strcmp(input,"!xosc-test deep") && SENSOR)) {
        // Simulate a stopped crystal for 90 s (also removes the DFLL reference, as a real stop
        // does). "deep" forces deep standby and detaches USB until the crystal is restored
        // (the next crystal retry, up to ~10 min after failover); read !status then.
        testForcesDeep = input[10] == ' ';
        if (testForcesDeep) {
          logLine("# xosc-test deep: USB detaching; crystal off 90 s in deep standby; USB returns after the crystal is restored (up to ~11 min)");
          Serial.flush(); delay(50);
          USBDevice.detach(); usbLive = false; usbDetachedForTest = true;
          usbSeen = false; updateSleepMode();
        } else logLine("# xosc-test: crystal oscillator disabled for 90 s; expect failover within ~8 s");
        xoscTestUntil = wallMs() + 90000; // while the RTC still runs
        SYSCTRL->XOSC32K.bit.ENABLE = 0;
      } else if (!strcmp(input,"!clock-retry")) {
        lastCrystalTryMs = wallMs() - CRYSTAL_RETRY_MS;
        logLine("# crystal retry requested");
      } else if (!strcmp(input,"!diag-reset")) {
        // Explicit diagnostic retention test only; never invoked automatically.
        traceStage(ST_MANUAL_RESET);
        NVIC_SystemReset();
      } else if (!strcmp(input,"!status")) {
        char c[21], r[21], p[21], msg[160];
        noInterrupts();
        uint64_t n = pulses.count, rev = pulses.revolutions, t = pulses.revolutionPeriod, e = rawEdges;
        interrupts();
        char re[21];
        decimal64(n,c); decimal64(rev,r); decimal64(t,p); decimal64(e,re);
        snprintf(msg,sizeof(msg),"# %s " FIRMWARE " radio=%s pin5=%d pulses=%s revolutions=%s rev_period_ticks=%s raw_edges=%s",
                 SENSOR ? "sensor" : "receiver",radioOK ? "OK" : "ERROR",digitalRead(REED),c,r,p,re);
        logLine(msg);
        noInterrupts();
        uint64_t tipsNow = rain.tips, tipEdgesNow = rainRawEdges;
        interrupts();
        decimal64(tipsNow,c); decimal64(tipEdgesNow,re);
        snprintf(msg,sizeof(msg),"# rain D10=%d tips=%s raw_edges=%s (1 tip per debounced closure; clean = 2 edges per tip) vbat=%lu mV",
                 digitalRead(RAIN),c,re,(unsigned long)readVbatMv());
        logLine(msg);
        snprintf(msg,sizeof(msg),"# baro BMP180 %s pressure=%ld Pa temp=%s%ld.%ld C errors=%lu last_good_ms_ago=%lu",
                 i2cOff ? "OFF" : baroOK ? "OK" : "absent",(long)baroPa,baroDeciC < 0 ? "-" : "",
                 (long)(abs(baroDeciC)/10),(long)(abs(baroDeciC)%10),(unsigned long)baroErrors,(unsigned long)(baroHave ? wallMs()-baroGoodMs : 0));
        logLine(msg);
        {
          const int32_t dc = int32_t(shtDeciK) - 2732;
          snprintf(msg,sizeof(msg),"# air SHT31 %s addr=0x%02x temp=%s%ld.%ld C rh=%u.%u %% errors=%lu last_good_ms_ago=%lu",
                   i2cOff ? "OFF" : shtAddr ? "OK" : "absent",shtAddr,dc < 0 ? "-" : "",
                   (long)(abs(dc)/10),(long)(abs(dc)%10),shtPermille/10,shtPermille%10,(unsigned long)shtErrors,
                   (unsigned long)(shtHave ? wallMs()-shtGoodMs : 0));
          logLine(msg);
        }
        if (i2cOff) { snprintf(msg,sizeof(msg),"# i2c OFF (re-checked every %lu s): %s",(unsigned long)(i2cRetryMs/1000),i2cOffReason); logLine(msg); }
        snprintf(msg,sizeof(msg),"# fw=" FIRMWARE " session=%08lx reset=0x%02x sleepprm=%u standby=%lu/%lu tx_ok=%lu tx_fail=%lu",
                 (unsigned long)session,resetCause,(unsigned)NVMCTRL->CTRLB.bit.SLEEPPRM,
                 (unsigned long)standbyEntries,(unsigned long)standbyReturns,
                 (unsigned long)txOK,(unsigned long)txFailed);
        logLine(msg);
        char diag[256], cmd[72];
        for (unsigned i=0; i<sizeof(cmd); ++i) cmd[i] = traceRecord.command[i];
        cmd[sizeof(cmd)-1] = 0;
        snprintf(diag,sizeof(diag),"# sleep=%s wdt_early=%lu stage=%s parent=%s since_ms=%lu last_command=%s",
                 deepSleep ? "deep" : "light",(unsigned long)wdtEarlyWakes,stageName(traceRecord.stage),stageName(traceRecord.parent),(unsigned long)traceRecord.sinceMs,cmd);
        logLine(diag);
        snprintf(diag,sizeof(diag),"# uart_ready=%u baud=%lu timeouts=%lu recoveries=%lu last_fault=%s injected=%u flags=0x%02lx status=0x%04lx ctrla=0x%08lx sync=0x%08lx",
                 radioUart.ready,(unsigned long)radioUart.baud,(unsigned long)radioUart.timeouts,
                 (unsigned long)radioUart.recoveries,radioUart.lastFault,radioUart.lastInjected,
                 (unsigned long)radioUart.lastFlags,(unsigned long)radioUart.lastStatus,
                 (unsigned long)radioUart.lastCtrlA,(unsigned long)radioUart.lastSync);
        logLine(diag);
        snprintf(diag,sizeof(diag),"# tx_dma_timeouts=%lu txc_inferred=%lu rtc_sync_timeouts=%lu compare_rearms=%lu radio_resets=%lu consecutive_fails=%lu wdt=16s",
                 (unsigned long)radioUart.dmaTimeouts,(unsigned long)radioUart.inferredCompletions,(unsigned long)rtcSyncTimeouts,
                 (unsigned long)compareRearms,(unsigned long)radioResets,(unsigned long)consecutiveFails);
        logLine(diag);
        snprintf(diag,sizeof(diag),"# clock=%s rtc_stuck=%u failovers=%lu restores=%lu stall_rearms=%lu lost_ms=%lu since_rtc_irq_ms=%lu",
                 onInternalClock ? "internal" : "crystal",(unsigned)rtcStuck,(unsigned long)clockFailovers,
                 (unsigned long)clockRestores,(unsigned long)rtcStallRearms,(unsigned long)clockLostMs,
                 (unsigned long)(millis() - lastRtcIrqMs));
        logLine(diag);
        if (previousTraceValid) {
          snprintf(diag,sizeof(diag),"# previous_trace session=%08lx stage=%s parent=%s since_ms=%lu command=%s",
                   (unsigned long)previousTrace.session,stageName(previousTrace.stage),stageName(previousTrace.parent),
                   (unsigned long)previousTrace.sinceMs,previousTrace.command);
          logLine(diag);
        } else logLine("# previous_trace unavailable (power loss, overwrite, or first diagnostic boot)");
      } else if (!strcmp(input,"!i2c-scan") && SENSOR) {
        if (i2cOff) {
          char msg[120]; snprintf(msg, sizeof(msg), "# i2c OFF: %s; fix the wiring (re-checked automatically) or try !i2c-probe", i2cOffReason);
          logLine(msg);
        }
        else i2cScan();
      } else if (!strcmp(input,"!sht") && SENSOR) {
        char msg[160];
        snprintf(msg, sizeof(msg), "# sht last probe: %s", shtProbeLog); logLine(msg);
        const bool ok = i2cReady(true) && shtBegin();
        snprintf(msg, sizeof(msg), "# sht probe now: %s -> %s", shtProbeLog, ok ? "found" : "absent"); logLine(msg);
        if (ok) {
          const bool rd = shtRead();
          if (rd) { shtHave = true; shtGoodMs = shtReadMs = wallMs(); } else ++shtErrors;
          snprintf(msg, sizeof(msg), "# sht %s", shtProbeLog); logLine(msg);
        }
      } else if (!strcmp(input,"!i2c-probe") && SENSOR) {
        i2cProbe();
      } else if (!strcmp(input,"!led on")) digitalWrite(LED_BUILTIN,HIGH);
      else if (!strcmp(input,"!led off")) digitalWrite(LED_BUILTIN,LOW);
    } else if (used < sizeof(input)-1) input[used++] = ch;
    else used = 0;
  }
}
