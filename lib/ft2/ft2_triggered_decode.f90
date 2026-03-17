subroutine ft2_triggered_decode(iwave, nqsoprogress, nfqso, nfa, nfb, &
     ndepth, ncontest, mycall, hiscall, &
     snr_out, dt_out, freq_out, msgbits_out, ndecoded, &
     known_bits, nknown, nfqso_only)

! Level 2: Sync-Triggered FT2 Decoder (JS8Call-Subspace variant)
! ==============================================================
! Two-phase architecture that eliminates DT grid search:
!
! Phase 1 - Costas Scan:
!   getcandidates2 + coarse sync2d over full DT range
!   Cost: O(candidates x time_steps x 4_dot_products) — FAST
!   Returns: list of (freq, ibest, sync) hits above trigger threshold
!
! Phase 2 - Targeted Decode:
!   Fine sync refinement + bitmetrics + LDPC — only at Phase 1 positions
!   DT is KNOWN from Phase 1, not searched
!   Cost: O(hits x LDPC) — LDPC only for confirmed sync
!
! Modified from Decodium 3.0 ASYMX: outputs raw message77 bits
! instead of formatted text, for native 72-bit JS8 framing.

  include 'ft2_params.f90'
  parameter (MAXCAND=300, NSS=NSPS/NDOWN, NDMAX=NMAX/NDOWN)
  parameter (MAXHITS=50, MAXDEC=20)

  integer*2 iwave(NMAX)
  integer nqsoprogress, nfqso, nfa, nfb, ndepth, ncontest
  character*12 mycall, hiscall

! Output arrays: up to MAXDEC decoded messages
  integer, intent(out) :: ndecoded
  integer, intent(out) :: snr_out(MAXDEC)
  real, intent(out) :: dt_out(MAXDEC)
  real, intent(out) :: freq_out(MAXDEC)
  integer*1, intent(out) :: msgbits_out(77, MAXDEC)

! Known (already-decoded) frames to skip — avoids re-decoding stale sync
  integer*1, intent(in) :: known_bits(77, MAXDEC)
  integer, intent(in) :: nknown

! nfqso_only=1: skip getcandidates2, use single candidate at nfqso
  integer, intent(in) :: nfqso_only

! Phase 1 hit storage
  real hit_freq(MAXHITS), hit_sync(MAXHITS), hit_snr(MAXHITS)
  integer hit_ibest(MAXHITS), hit_idf(MAXHITS)
  integer nhits

! Decode variables
  real dd(NMAX)
  real candidate(2,MAXCAND), savg(NH1), sbase(NH1)
  complex cd2(0:NDMAX-1), cb(0:NDMAX-1), cd(0:NN*NSS-1)
  complex ctwk(2*NSS), ctwk2(2*NSS,-16:16)
  real bitmetrics(2*NN,3)
  real llr(2*ND), llra(2*ND), llrb(2*ND), llrc(2*ND)
  real llrd(2*ND), llre(2*ND)
  real a(5)

  integer*1 message77(77), rvec(77), apmask(2*ND), cw(2*ND)
  integer*1 message91(91)
  integer*1 hbits(2*NN)
  integer*1 prev_bits(77, MAXDEC)
  integer ncand, ndepth0
  logical badsync, dobigfft, doosd
  logical first
  real xibest, sm1, sp1, den

  data rvec/0,1,0,0,1,0,1,0,0,1,0,1,1,1,1,0,1,0,0,0,1,0,0,1,1,0,1,1,0, &
     1,0,0,1,0,1,1,0,0,0,0,1,0,0,0,1,0,1,0,0,1,1,1,1,0,0,1,0,1, &
     0,1,0,1,0,1,1,0,1,1,1,1,1,0,0,0,1,0,1/
  data first/.true./
  save first, ctwk2

  ndecoded = 0
  snr_out = 0
  dt_out = 0.0
  freq_out = 0.0
  msgbits_out = 0

! Initialize frequency tweak factors (once)
  if(first) then
    do idf = -16, 16
      a = 0.
      a(1) = real(idf)
      ctwk = 1.
      call twkfreq1(ctwk, 2*NSS, (12000.0/NDOWN)/2.0, a, ctwk2(:,idf))
    enddo
    first = .false.
  endif

  ndepth0 = iand(ndepth, 7)
  doosd = ndepth0.ge.3
  dd = iwave

! ============================================
! PHASE 1: Fast Costas Sync Scan
! ============================================
  nhits = 0

! Get candidate frequencies
  if(nfqso_only.eq.1) then
! L2 mode: skip getcandidates2 spectrogram (308 FFTs + baseline)
! Just use nfqso as the single candidate — we know the frequency
    ncand = 1
    candidate(1,1) = real(nfqso)
    candidate(2,1) = 0.0
  else
    ! TEMPORARY: threshold raised from 0.50 to 0.60 to reduce false candidates on noise
    call getcandidates2(dd, real(nfa), real(nfb), 0.60, nfqso, MAXCAND, &
         savg, candidate, ncand, sbase)
  endif

  write(*,'(A,I4,A,I5,A,I5,A,I2)') '[FT2-L2] Phase1: ncand=', ncand, &
       ' nfqso=', nfqso, ' ndepth=', ndepth0, ' nfqso_only=', nfqso_only
  if(ncand.gt.0) write(*,'(A,5F8.1)') '[FT2-L2] cand freqs:', &
       (candidate(1,min(i,ncand)), i=1,min(5,ncand))

! For each candidate, coarse sync search only
  dobigfft = .true.
  do icand = 1, ncand
    f0 = candidate(1, icand)
    snr0 = candidate(2, icand) - 1.0

    call ft2_downsample(dd, dobigfft, f0, cd2)
    if(dobigfft) dobigfft = .false.

    sum2 = sum(cd2*conjg(cd2))/(real(NMAX)/real(NDOWN))
    if(sum2.gt.0.0) cd2 = cd2/sqrt(sum2)

! Coarse sync search: full DT range, freq +/-12 Hz step 3
! DT step 8 (was 4): halves Phase 1 cost, sync2d pattern is 128 samples wide
    ibest_c = -1
    idfbest_c = 0
    smax_c = -99.
    do idf = -12, 12, 3
      do istart = -688, NDMAX-NN*NSS+320, 8
        call sync2d(cd2, istart, ctwk2(:,idf), 1, sync)
        if(sync.gt.smax_c) then
          smax_c = sync
          ibest_c = istart
          idfbest_c = idf
        endif
      enddo
    enddo

! Trigger threshold (lower than decode threshold — catch weak signals)
    syncmin_scan = 0.60
    if(ndepth0.ge.3) syncmin_scan = 0.50

    if(smax_c.ge.syncmin_scan .and. nhits.lt.MAXHITS) then
      nhits = nhits + 1
      hit_freq(nhits) = f0
      hit_ibest(nhits) = ibest_c
      hit_idf(nhits) = idfbest_c
      hit_sync(nhits) = smax_c
      hit_snr(nhits) = snr0
      write(*,'(A,I3,A,F7.1,A,F5.2,A,I5,A,I3)') &
           '[FT2-L2] Phase1 HIT #', nhits, ' f=', f0, &
           ' sync=', smax_c, ' ibest=', ibest_c, ' idf=', idfbest_c
    endif
  enddo

  write(*,'(A,I3)') '[FT2-L2] Phase1 total hits=', nhits

! ============================================
! PHASE 2: Targeted Decode at Sync Positions
! ============================================
  do ihit = 1, nhits
    if(ndecoded.ge.MAXDEC) exit

    f0 = hit_freq(ihit)
    ibest0 = hit_ibest(ihit)
    idf0 = hit_idf(ihit)
    snr0 = hit_snr(ihit)

! Re-downsample at candidate frequency (reuses cached FFT)
    call ft2_downsample(dd, .false., f0, cd2)
    sum2 = sum(cd2*conjg(cd2))/(real(NMAX)/real(NDOWN))
    if(sum2.gt.0.0) cd2 = cd2/sqrt(sum2)

! Fine sync: +/-8 samples, +/-4 Hz around coarse position
! DT is KNOWN from Phase 1 — just refine it
! Wider than original ±5 to compensate for Phase 1 coarser DT step (8 vs 4)
    ibest = ibest0
    idfbest = idf0
    smax = -99.
    do idf = max(-16, idf0-4), min(16, idf0+4)
      do istart = ibest0-8, ibest0+8
        call sync2d(cd2, istart, ctwk2(:,idf), 1, sync)
        if(sync.gt.smax) then
          smax = sync
          ibest = istart
          idfbest = idf
        endif
      enddo
    enddo

! Decode threshold (stricter than scan trigger)
    smaxthresh = 0.80
    if(ndepth0.ge.3) smaxthresh = 0.65
    if(smax.lt.smaxthresh) then
      write(*,'(A,I3,A,F7.1,A,F5.2,A,F5.2)') &
           '[FT2-L2] Phase2 REJECT sync: hit#', ihit, ' f=', f0, &
           ' smax=', smax, ' thresh=', smaxthresh
      cycle
    endif

! Corrected frequency
    f1 = f0 + real(idfbest)
    if(f1.le.10.0 .or. f1.ge.4990.0) cycle

! Final downsample at corrected frequency
    call ft2_downsample(dd, .false., f1, cb)
    sum2 = sum(abs(cb)**2)/real(NDMAX)
    if(sum2.gt.0.0) cb = cb/sqrt(sum2)

! Extract signal at known position (DT eliminated)
    cd = 0.
    if(ibest.ge.0) then
      it = min(NDMAX-1, ibest+NN*NSS-1)
      np = it - ibest + 1
      cd(0:np-1) = cb(ibest:it)
    else
      cd(-ibest:ibest+NN*NSS-1) = cb(0:NN*NSS+2*ibest-1)
    endif

! Bit metrics
    call get_ft2_bitmetrics(cd, bitmetrics, badsync)
    if(badsync) then
      write(*,'(A,I3,A,F7.1,A,F5.2)') &
           '[FT2-L2] Phase2 REJECT badsync: hit#', ihit, &
           ' f=', f1, ' smax=', smax
      cycle
    endif

! Sub-sample DT refinement via parabolic interpolation
    if(ibest.gt.0 .and. ibest.lt.NDMAX-1) then
      call sync2d(cd2, ibest-1, ctwk2(:,idfbest), 1, sm1)
      call sync2d(cd2, ibest+1, ctwk2(:,idfbest), 1, sp1)
      den = sm1 - 2.0*smax + sp1
      if(abs(den).gt.1.0e-6) then
        xibest = real(ibest) + 0.5*(sm1 - sp1)/den
      else
        xibest = real(ibest)
      endif
    else
      xibest = real(ibest)
    endif

! Sync quality check (hard Costas symbol verification)
    hbits = 0
    where(bitmetrics(:,1).ge.0) hbits = 1
    ns1 = count(hbits(  1:  8).eq.(/0,0,0,1,1,0,1,1/))
    ns2 = count(hbits( 67: 74).eq.(/0,1,0,0,1,1,1,0/))
    ns3 = count(hbits(133:140).eq.(/1,1,1,0,0,1,0,0/))
    ns4 = count(hbits(199:206).eq.(/1,0,1,1,0,0,0,1/))
    nsync_qual = ns1 + ns2 + ns3 + ns4
    nsync_qual_min = 20
    if(ndepth0.ge.3) nsync_qual_min = 18
    if(nsync_qual.lt.nsync_qual_min) then
      write(*,'(A,I3,A,F7.1,A,I3,A,I3)') &
           '[FT2-L2] Phase2 REJECT nsync: hit#', ihit, &
           ' f=', f1, ' nsync=', nsync_qual, ' min=', nsync_qual_min
      cycle
    endif
    write(*,'(A,I3,A,F7.1,A,I3,A,F5.2,A,I5)') &
         '[FT2-L2] Phase2 PASS: hit#', ihit, ' f=', f1, &
         ' nsync=', nsync_qual, ' smax=', smax, ' ibest=', ibest

! Scale LLRs from bitmetrics (3 metric types)
    scalefac = 2.83
    llra(  1: 58) = bitmetrics(  9: 66, 1)
    llra( 59:116) = bitmetrics( 75:132, 1)
    llra(117:174) = bitmetrics(141:198, 1)
    llra = scalefac * llra
    llrb(  1: 58) = bitmetrics(  9: 66, 2)
    llrb( 59:116) = bitmetrics( 75:132, 2)
    llrb(117:174) = bitmetrics(141:198, 2)
    llrb = scalefac * llrb
    llrc(  1: 58) = bitmetrics(  9: 66, 3)
    llrc( 59:116) = bitmetrics( 75:132, 3)
    llrc(117:174) = bitmetrics(141:198, 3)
    llrc = scalefac * llrc

! Multi-metric: best-of (max |value|) and average
    do i = 1, 2*ND
      if(abs(llra(i)).ge.abs(llrb(i)) .and. &
         abs(llra(i)).ge.abs(llrc(i))) then
        llrd(i) = llra(i)
      elseif(abs(llrb(i)).ge.abs(llrc(i))) then
        llrd(i) = llrb(i)
      else
        llrd(i) = llrc(i)
      endif
      llre(i) = (llra(i) + llrb(i) + llrc(i))/3.0
    enddo

! 5 metric passes — try best-of and average first (highest success rate)
    do ipass = 1, 5
      if(ipass.eq.1) llr = llrd    ! best-of-3 (most likely to succeed)
      if(ipass.eq.2) llr = llre    ! average-of-3
      if(ipass.eq.3) llr = llra    ! 1-symbol coherence
      if(ipass.eq.4) llr = llrb    ! 2-symbol coherence
      if(ipass.eq.5) llr = llrc    ! 4-symbol coherence
      apmask = 0

      message77 = 0
      dmin = 0.0
      maxosd = 2
      if(ndepth0.ge.3) maxosd = 3
      if(.not.doosd) maxosd = -1

      Keff = 91
      call decode174_91(llr, Keff, maxosd, 3, apmask, message91, cw, &
                        ntype, nharderror, dmin)
      message77 = message91(1:77)

      if(sum(message77).eq.0) cycle
      if(nharderror.lt.0 .and. ipass.ge.5) then
        write(*,'(A,I3,A,F7.1,A,I3)') &
             '[FT2-L2] Phase2 LDPC FAIL: hit#', ihit, &
             ' f=', f1, ' nharderror=', nharderror
      endif
      if(nharderror.ge.0) then
        message77 = mod(message77 + rvec, 2)

! Check duplicate within this call AND against known frames
        idupe = 0
        do i = 1, ndecoded
          if(all(prev_bits(:,i).eq.message77)) idupe = 1
        enddo
        do i = 1, min(nknown, MAXDEC)
          if(all(known_bits(:,i).eq.message77)) idupe = 1
        enddo
        if(idupe.eq.1) exit

! Store decoded result: raw bits + metadata
        ndecoded = ndecoded + 1
        msgbits_out(:, ndecoded) = message77
        prev_bits(:, ndecoded) = message77

! Calculate SNR
        if(snr0.gt.0.0) then
          xsnr = 10*log10(snr0) - 13.0
        else
          xsnr = -21.0
        endif
        snr_out(ndecoded) = nint(max(-21.0, xsnr))

! DT from known sync position
        dt_out(ndecoded) = xibest/1333.33 - 0.5
        freq_out(ndecoded) = f1
        exit
      endif
    enddo  ! ipass
  enddo  ! ihit

  return
end subroutine ft2_triggered_decode
