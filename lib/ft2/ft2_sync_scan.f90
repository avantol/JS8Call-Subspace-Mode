subroutine ft2_sync_scan(iwave, nfreqs, freqs, sync_out, freq_out, &
     ibest_out, idf_out)

! Lightweight sync scanner: downsample + sync2d at a frequency grid.
! Returns best sync quality, frequency, time position for each input freq.
! Used by the C++ sync monitor to detect Costas tones before full decode.

  include 'ft2_params.f90'
  parameter (NSS=NSPS/NDOWN, NDMAX=NMAX/NDOWN)

  integer*2 iwave(NMAX)
  integer, intent(in) :: nfreqs
  real, intent(in) :: freqs(nfreqs)
  real, intent(out) :: sync_out       ! best sync across all freqs
  real, intent(out) :: freq_out       ! frequency of best sync
  integer, intent(out) :: ibest_out   ! best time position (downsampled)
  integer, intent(out) :: idf_out     ! best freq offset index

  real dd(NMAX)
  complex cd2(0:NDMAX-1)
  complex ctwk(2*NSS), ctwk2(2*NSS,-16:16)
  logical first, dobigfft
  save first, ctwk2
  data first/.true./

! One-time init: precompute frequency tweak factors
  if(first) then
    twopi = 8.0*atan(1.0)
    do i = 1, 2*NSS
      phi = (i-1)*twopi/(2*NSS)
      ctwk(i) = cmplx(cos(phi), sin(phi))
    enddo
    do idf = -16, 16
      a = (idf * 0.5) / (12000.0/NDOWN)
      call twkfreq1(ctwk, 2*NSS, (12000.0/NDOWN)/2.0, a, ctwk2(:,idf))
    enddo
    first = .false.
  endif

! Convert int16 to real
  dd = real(iwave)

! Initialize outputs
  sync_out = -99.0
  freq_out = 0.0
  ibest_out = -1
  idf_out = 0

  dobigfft = .true.
  do ifreq = 1, nfreqs
    f0 = freqs(ifreq)

    call ft2_downsample(dd, dobigfft, f0, cd2)
    if(dobigfft) dobigfft = .false.

    ! Normalize
    sum2 = sum(cd2*conjg(cd2))/(real(NMAX)/real(NDOWN))
    if(sum2.gt.0.0) cd2 = cd2/sqrt(sum2)

    ! Coarse sync: DT step 8, freq offset ±12 step 3 (same as Phase 1)
    do idf = -12, 12, 3
      do istart = -688, NDMAX-NN*NSS+320, 8
        call sync2d(cd2, istart, ctwk2(:,idf), 1, sync)
        if(sync.gt.sync_out) then
          sync_out = sync
          freq_out = f0
          ibest_out = istart
          idf_out = idf
        endif
      enddo
    enddo
  enddo

  return
end subroutine ft2_sync_scan
