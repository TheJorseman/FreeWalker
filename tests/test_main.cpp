// WalkerCore — unit test entry point.
//
// Pulls in every per-module test entry function (TestXxx). Tests link
// against the same domain + provider sources the DLL does, so any code
// path we touch in the DLL we can exercise here.

#include "TestHarness.h"

#include <stdio.h>

namespace walkercore {

// Each test file exposes a `void TestName();` function declared here.
void TestCameraViewMatrix();
void TestCameraProjectionMatrix();
void TestCameraAdoptPlayer();
void TestCameraStateUpdateEveryFieldHasObservableEffect();
void TestCameraStateUpdatePerfBudget();
void TestCameraStateProjectionCacheHitWhenInputsUnchanged();
void TestCameraStateProjectionCacheInvalidatedByEachInput();
void TestCameraStateUpdateCachedPathPerfBudget();
void TestCameraStateViewRotationCacheHitWhenInputsUnchanged();
void TestCameraStateViewRotationCacheInvalidatedByYaw();
void TestCameraStateViewRotationCacheInvalidatedByPitch();
void TestCameraStateViewRotationCacheNegativeControlOnEyePosition();
void TestCameraStateViewRotationCacheExplicitDirtyFlag();
void TestCameraStateViewRotationCacheFullBuildMatchesSplit();
void TestCameraStateUpdateFullCacheHitPerfBudget();
void TestMovementConfigDefaults();
void TestMovementConfigEveryFieldHasObservableEffect();
void TestTruckLocalRoundtrip();
void TestTruckLocalWorldIdentity();
void TestTruckLocalTransformEveryFieldHasObservableEffect();
void TestIniParserBasic();
void TestIniParserKeys();
void TestIniParserComments();
void TestIniParserUnknownKeys();
void TestIniRoundtrip();
void TestIniHeadTrackerSectionParsed();
void TestIniHeadTrackerUnknownKeyIgnored();
// M2 lander INI gates — `[m2] d3d11_hook` + `[m2] d3d12_hook`. Default
// `false` ships the worst-known ATS 1.61.3.1s + ReShade + OptiScaler +
// RenoDX stack as the safe baseline. LoadFromPath auto-migrates a
// pre-existing INI that still has the OLD default of `true`, so users
// upgrading from a build that had `true` as the default don't keep
// hitting the amtrucks.exe+0x1d5300a `movdqa` #GP after the upgrade.
void TestIniM2D3DHookGatesParseAndDefault();
void TestIniM2D3DHookGatesUnknownKeyIgnored();
void TestIniM2D3DHookGatesAutoMigratesOldTrueValues();
// M2 long-term + M3 lander INI gates — same rationale as the d3d11/
// d3d12 gates but for the in-process MinHook trampolines on
// amtrucks.exe. Default `false` for both.
void TestIniM2CameraHijackInstallDefaultIsFalse();
void TestIniM3PxSceneHookInstallDefaultIsFalse();
void TestIniM2CameraHijackInstallParseAndRoundTrip();
void TestIniM3PxSceneHookInstallParseAndRoundTrip();
void TestIniM2CameraHijackInstallAutoMigratesOldTrueValues();
void TestIniM3PxSceneHookInstallAutoMigratesOldTrueValues();
void TestIniM2GameFreeCameraShimDefaultIsFalse();
void TestIniM2GameFreeCameraShimAutoMigratesOldTrueToFalse();
void TestIniM2GameFreeCameraShimExplicitFalseIsPreserved();
// M2 dev_camera_toggle_vk — virtual key the shim synthesises on
// every Walking entry. SCS hard-codes the dev free camera toggle to
// the F11 key (VK_F11 = 0x7A), gated by `g_developer=1` AND
// `g_console=1` in config.cfg. (M2.7 default after the live
// dev-free-camera-shortcut change; earlier defaults VK_NUMPAD0
// 0x60 and VK_0 0x30 were RE-driven guesses that did not work in
// amtrucks.exe 1.61.3.1s.)
void TestIniM2DevCameraToggleVkDefaultIsVKF11();
void TestIniM2DevCameraToggleVkParsesHexAndDecimal();
void TestIniM2DevCameraToggleVkOutOfRangeKeepsDefault();
void TestIniM2DevCameraToggleVkUnparseableKeepsDefault();
void TestIniM2DevCameraToggleVkRoundTripsThroughSaveToPath();
void TestIniM2SentinelSuppressesExplicitFlagForAutoWrittenFalse();
void TestIniM2NoSentinelMeansExplicitTrue();
void TestIniM2SentinelForDevCameraToggleVk();
void TestIniSaveToPathEmitsSentinelForDefaultValues();
void TestIniSaveToPathOmitsSentinelForNonDefaultValues();
void TestIniM2LegacyIniGetsSentinelPatchedInPlace();
void TestIniM2LegacyPatchSkipsNonDefaultValues();
void TestApplyAllPushesEveryFieldToEveryConsumer();
void TestWalkerFsmInitialState();
void TestWalkerFsmDrivingToWalking();
void TestWalkerFsmWalkingToDriving();
void TestWalkerFsmF10FromWalkingExits();
void TestWalkerFsmBindPlayerTarget();
void TestWalkerFsmNoPlayerTargetIsSafe();
void TestWalkerFsmShouldRenderOverridePerState();
void TestWalkerFsmTransitionProgressMonotonic();
void TestWalkerFsmInteractOutOfRangeIsSilent();
void TestWalkerFsmStateNameReturnsNameForEveryValue();
void TestWalkerFsmStateNamesAreDistinct();
void TestWalkerFsmInputsEveryFieldHasObservableEffect();
void TestWalkerFsmTransitionTargetMath();
void TestFrameLoopIntegration();
void TestFrameLoopCameraOverride();
void TestFrameLoopNoFsmIsNoop();
void TestFrameLoopDiagonalMotionIsSpeedBounded();
void TestFrameLoopCrouchWinsOverRun();
void TestPhaseAnimatePlayerSkipsSinCosWhenNotMoving();
void TestPhaseAnimatePlayerBobAppliesWhenMoving();
void TestPhaseAnimatePlayerBobDecaysWhenStationary();
void TestPhaseAnimatePlayerZeroAmplitudeIsSilent();
void TestFrameLoopSetAspectIsNoopWhenUnchanged();
void TestPhaseAnimatePlayerKeepsHeadBobPhaseBounded();
void TestPhaseIntegrateMovementSkipsSqrtWhenIntentIsOnAxis();
void TestPhaseIntegrateMovementClampsVerticalVelocity();
void TestMovementConfigVerticalVelocityCapDefault();
// Y-clamp floor probes — keep the player anchored to the truck's
// ground reference until M3 lander ships real PhysX ground
// collision. Without this, the integrator's gravity drops the player
// to Y = -∞ during a long Walking session and the SCS developer free
// camera the GameFreeCameraShim dispatches on F10 ends up at
// eye.Y ≈ -800 m (under the world) so the user sees no camera change.
void TestFrameLoopFloorClampDefaultsToZero();
void TestFrameLoopFloorClampSnapsPlayerUp();
void TestFrameLoopFloorClampMarksGrounded();
void TestFrameLoopFloorClampIgnoresPlayerAboveFloor();
void TestFrameLoopFloorClampHandlesFallingPlayer();
void TestFrameLoopRunOnceDoesNotClobberConductorInput();
void TestFrameLoopRunOnceSamplesWhenConductorDidNot();
void TestFrameLoopPlayerSnapshotStableAcrossConductor();
void TestFrameLoopOverridePathPreservesProjectionFields();
void TestFrameLoopWalkingPopulatesViewAndProjectionMatrices();
void TestFrameLoopHeadTrackerNotInitialisedByDefault();
void TestFrameLoopInitHeadTrackerIsIdempotent();
void TestFrameLoopHeadTrackerShutdownRestoresBaseline();
void TestFrameLoopHeadTrackerTickIsNoOpWithoutReceiver();
void TestFrameLoopHeadTrackerStateDefaultSourceIsSentinel();
void TestM7LanderMath_MouseOnlyPathKeepsCameraEqualToPlayer();
void TestM7LanderMath_HeadTrackerDoesNotMutatePlayerYaw();
void TestM7LanderMath_HeadTrackerOffsetAddsToCameraYaw();
void TestM7LanderMath_HeadTrackerOffsetAddsToCameraPitch();
void TestM7LanderMath_PitchClampedWhenOffsetExceedsLimitMax();
void TestM7LanderMath_ShutdownReturnsCameraToBodyYaw();
void TestM7LanderMath_TrigCacheInvalidatedWhenOffsetAdded();
void TestWalkerFsmInputsPointerRefactorContract();
void TestWalkerFsmTransitionOutCameraFromMatchesCabinSeatWhenPointerAliases();
void TestCameraStateBuildViewRotationPitchZeroSkipsNormalisation();
void TestCameraStatePrecomputedYawTrigCacheContract();
void TestCameraStateResetClearsPrecomputedYawTrigCache();
void TestFrameLoopRunOnceHotPathPerfBudget();
void TestPerFrameTickConductorPerfBudget();
void TestInputHookSamplePerfBudget();
void TestKnockoutThreshold();
void TestKnockoutRunningDoesNotTrigger();
void TestKnockoutFastImpactTriggers();
void TestKnockoutProviderStateCycleAndEveryField();
void TestKnockoutTeleportReprime();
void TestKnockoutRecoveryResetsLastPositionBaseline();
void TestKnockoutPrimeResetsTrackingBaseline();
void TestKnockoutConfigDefaultValues();
void TestKnockoutPathologicalDtGuards();
// M10 Lander: ||player.velocity|| as the trigger metric (replaces
// the legacy dist/dt formula that misread position glitches as
// 50-100 m/s impacts).
void TestKnockoutTriggerUsesIntegratedVelocityNotDistDt();
void TestKnockoutTriggerFiresOnIntegratedVelocityOnly();
void TestKnockoutTriggerUsesMagnitudeAcrossAxes();
void TestKnockoutStationaryPlayerNeverTriggers();
void TestDoorAnimEasing();
void TestDoorAnimProviderEveryFieldHasObservableEffect();
void TestDoorAnimWiringInitialStateNotAnimating();
void TestDoorAnimWiringDriveToExitOpensDoor();
void TestDoorAnimWiringWalkToEnterClosesDoor();
void TestDoorAnimWiringDoorTweenCompletes();
void TestDoorAnimWiringTickAdvancesRegardlessOfState();
void TestDoorAnimWiringStopCancelsTween();
void TestDoorAnimWiringDumpReportsNotInitialized();
void TestDoorAnimWiringDumpReportsIdleAngle();
void TestDoorAnimWiringDumpReportsAnimating();
void TestDoorAnimWiringTransitionStartToAngleAtTarget();
void TestInputHookRisingEdgeVisible();
void TestInputHookRisingEdgeConsumedAfterSample();
void TestInputHookFallingEdgeVisible();
void TestInputHookKeyRepeatNoExtraEdges();
void TestInputHookMultipleKeysIndependent();
void TestInputHookDumpStateKeyDefaultIsF12();
void TestInputHookDumpStateRisingEdgeConsumed();
void TestInputHookDumpStateFallingEdgeVisible();
void TestInputHookClassifyToggleWalkAlone();
void TestInputHookClassifyToggleWalkWithCrouchHeld();
void TestWalkercoreKeyNameReturnsNameForEveryValue();
void TestWalkercoreKeyRangeIsContiguous();
void TestInputHookOnRawInputMotionAccumulates();
void TestInputHookOnRawInputMotionDrainsOnNextSample();
void TestInputHookOnRawInputAbsoluteMotionIsDropped();
void TestInputHookOnRawInputWheelAccumulates();
void TestInputHookOnRawInputWheelDrainsOnNextSample();
void TestInputHookOnRawInputLeftButtonPress();
void TestInputHookOnRawInputLeftButtonRelease();
void TestInputHookOnRawInputRightButtonPress();
void TestInputHookOnRawInputButtonHeldNoExtraEdges();
void TestInputHookOnRawInputIgnoresNonMouse();
void TestInputHookOnRawInputCombinedMotionAndButton();
void TestWindowSubclassFreshStateIsNotInstalled();
void TestWindowSubclassInstallOnNullHwndIsRefused();
void TestWindowSubclassInstallUninstallRoundTrip();
void TestWindowSubclassInstallIsIdempotent();
void TestWindowSubclassUninstallWhenNotInstalledIsSafe();
void TestWindowSubclassUninstallWithMismatchedHwndIsIgnored();
void TestWindowSubclassReinstallReplacesHwnd();
void TestWindowSubclassRawInputSinkReceivesWM_INPUT();
void TestWindowSubclassRawInputSinkReceivesMultipleWM_INPUT();
void TestWindowSubclassClearedSinkDropsWM_INPUT();
void TestWindowSubclassSinkSurvivesReinstall();
void TestWindowSubclassChainsToOriginalForNonWM_INPUT();
void TestWindowSubclassChainsToOriginalForMultipleMessages();
void TestWindowSubclassWM_INPUTAlsoChainsToOriginal();
void TestWindowSubclassRestoresOriginalOnUninstall();
void TestHeadTrackerSteadyStateConverges();
void TestHeadTrackerNoInputDoesNotDrift();
void TestHeadTrackerNegativeDeltaReturns();
void TestHeadTrackerDisabledSourceIgnored();
void TestHeadTrackerSourceNameReturnsNameForEveryValue();
void TestHeadTrackerSourceRangeIsContiguous();
void TestHeadTrackerProviderEveryFieldHasObservableEffect();

void TestHeadTrackerWiringDefaultConfigAllDisabled();
void TestHeadTrackerWiringFreshProviderReportsInactive();
void TestHeadTrackerWiringInstallIdempotent();
void TestHeadTrackerWiringSourceNameAllValues();
void TestHeadTrackerWiringDumpReportsNotInitialized();
void TestHeadTrackerWiringDumpReportsInactiveByDefault();
void TestHeadTrackerWiringDumpReportsActiveAfterMouseFeed();
void TestHeadTrackerWiringDumpReportsYawAfterMouseFeed();
void TestHeadTrackerWiringDumpUpdatesOnFeed();
void TestHeadTrackerWiringConfigOverride();
void TestHeadTrackerWiringPeekDoesNotAdvance();
void TestHeadTrackerWiringPeekIsConst();

void TestShadowWiringDefaultSunDirection();
void TestShadowWiringComputeActiveWithFeet();
void TestShadowWiringComputeInactiveWithoutFeet();
void TestShadowWiringComputeRadiusIsPositive();
void TestShadowWiringSetConfigAppliesImmediately();
void TestShadowWiringSunOnHorizonIsSafe();
void TestShadowWiringDumpReportsNotInitialized();
void TestShadowWiringDumpReportsNoPlayerState();
void TestShadowWiringDumpReportsActiveWithFrameLoopAndPlayer();
void TestShadowProviderComputeEveryFieldHasObservableEffect();

void TestFlashlightInitialStateOff();
void TestFlashlightToggleFlips();
void TestFlashlightSetOnExplicit();
void TestFlashlightOffHasZeroIntensity();
void TestFlashlightOnHasUnitIntensity();
void TestFlashlightComputeOriginMatchesCamera();
void TestFlashlightComputeDirectionMatchesCameraForward();
void TestFlashlightDirectionIsNormalised();
void TestFlashlightConeAnglesArePositive();
void TestFlashlightDumpReportsNotInitializedWhenUnwired();
void TestFlashlightDumpReportsOffWhenProviderOff();
void TestFlashlightDumpReportsOnWhenProviderOn();
void TestFlashlightDumpMirrorsToggle();

// SDK-free architectural invariants.
void TestGameIdentityFromFields();
void TestGameIdentityFromFieldsAts();
void TestGameIdentityFromFieldsNullGameId();
void TestGameIdentityEquality();
void TestGameIdentityMajorMinorHighBits();

// VersionTable / Feature enum coverage probes — pin "every Feature
// bit has a name and a switch case" as a compile-time + unit-test
// invariant.
void TestFeatureNameReturnsNameForEveryValue();
void TestFeatureBitsAreUniquePowersOfTwo();
void TestFeatureProbeTableHasExpectedCount();
void TestAtS1_61_3_1sLookupMatchesSteamBuild();
void TestVersionTableUnknownBuildReturnsCatchAll();
void TestVersionTableEveryDocumentedBuildResolves();

// Settings INI Save/Load regression — guards the dump_state_vk /
// zoom_mouse_button / flashlight_toggle_mouse_button round-trip
// that previously caused "F12 dumps nothing" reports.
void TestIniDumpStateVkRoundTrip();
void TestIniSaveEmitsDumpStateVkAndMouseButtons();
void TestIniMissingKeysDefaultIsPreserved();

// ABI contract: LoadLibrary + GetProcAddress for the 14 documented
// public exports of libWalkerCore.dll. See tests/test_abi_contract.cpp.
void TestAbiContractDllLoads();

// Honest-stub one-shot warning probes.
void TestPhysXStubWarnsExactlyOnce();
void TestKnockoutStubWarnsExactlyOnce();
void TestCouplingStubWarnsExactlyOnce();
void TestPolishStubWarnsExactlyOncePerName();
void TestFuelPumpStubWarnsExactlyOnce();

// M3 PxScene vtable memory-scan probes.
void TestPhysXMemoryScanSyntheticVtable();
void TestPhysXMemoryScanRejectsShortBuffer();
void TestPhysXMemoryScanRejectsNullBuffer();
void TestPhysXMemoryScanRejectsAllZeroBuffer();

// M3 multi-strategy detection probes — strings + signature combined.
void TestPhysXStringsProbeFindsKnownString();
void TestPhysXStringsProbeFindsAllKnownStrings();
void TestPhysXStringsProbeRejectsEmptyBuffer();
void TestPhysXStringsProbeIgnoresPartialMatch();
void TestPhysXSignatureRunAcceptedAtThreshold();
void TestPhysXSignatureRunRejectedBelowThreshold();
void TestPhysXBothStrategiesHybrid();
void TestPhysXStrategyTagIsCanonical();
void TestPhysXDumpVtableEntriesReportsFirstNVtableRun();
void TestPhysXDumpVtableEntriesNoMatchReturnsZero();
void TestPhysXDumpVtableEntriesRejectsNullBuffer();
void TestPhysXDumpVtableEntriesAnnotatedLabelsCatalogMatches();
void TestPhysXResolveKnownMethodsFindsReleaseAtSlotZero();
void TestPhysXResolveKnownMethodsAllEightSlotsInOrder();
void TestPhysXResolveKnownMethodsNoMatchReturnsFalse();
void TestPhysXResolveKnownMethodsRespectsBounds();
void TestPhysXKnownMethodsCatalogIsComplete();
void TestPhysXKnownMethodsSweepIsTheHighestVA();
void TestPhysXKnownMethodsCatalogVAsInPhysXRange();
void TestPhysXKnownMethodsCatalogNamesMatchSDK();
void TestPhysXKnownMethodsCatalogOrderIsStable();
void TestPhysXSweepSecondaryResolvesCatalogSweepAtSlotFour();
void TestPhysXSweepSecondaryReturnsNegativeOneWhenAbsent();
void TestPhysXSweepSecondaryMatchesWithinTolerance();
void TestPhysXSweepSecondaryRejectsOffsetBeyondTolerance();
void TestPhysXSweepSecondaryHonoursOffset();
void TestPhysXSweepSecondaryConstantsMatchCatalog();
#ifdef WALKERCORE_REAL_BINARY_TEST_PATH
void TestPhysXRealBinaryMatchesCatalog();
void TestPhysXRealBinaryRuntimeScanPath();
#endif

// M9 SCS configuration event + CouplingProvider real-hook probes.
void TestScsTelemetryProviderRegisterEventsNullIsSafe();
void TestScsTelemetryProviderUnregisterEventsNullIsSafe();
void TestCouplingProviderPlaceholderWaypointWhenHookUnset();
void TestCouplingProviderRealHookPositionTransformsToWorld();
void TestCouplingProviderRealHookPositionRotatesWithTruckHeading();
void TestCouplingProviderRealHookLandingGearStep();
void TestCouplingProviderHookUnsetUsesTruckLocalFallback();
// M9 Lander: telemetry-driven ForceComplete via OnTrailerAttachStateChanged.
void TestCouplingOnTrailerAttachIdleIsNoOp();
void TestCouplingOnTrailerAttachCompleteIsNoOp();
void TestCouplingOnTrailerAttachTrueMidFsmForcesComplete();
void TestCouplingOnTrailerAttachFalseMidFsmResetsToIdle();
void TestCouplingOnTrailerAttachIdempotentAcrossIdenticalTransitions();

// M7 opentrack UDP receiver probes.
void TestOpentrackReceiverInstallUninstallIdempotent();
void TestOpentrackReceiverTickWithoutInstallIsSafe();
void TestOpentrackReceiverTickWithNullProviderDrainsSocket();
void TestOpentrackReceiverRoundTripFeedsDeltasIntoProvider();
void TestOpentrackReceiverShortestArcDeltaOnYawWrap();

// M5 TruckDoorConfigFromTruckState probes.
void TestTruckDoorConfigFromStateFallsBackWhenAnchorMissing();
void TestTruckDoorConfigFromStateAnchorsDoorToCabin();
void TestTruckDoorConfigFromStateDriverSeatFromHeadPosition();
void TestTruckDoorConfigFromStateOnlyCabinSetFallsBack();

// M5 PerTruckDoorTable probes (door geometry per truck.model_id).
void TestPerTruckDoorTableBuiltInFindsKnownRow();
void TestPerTruckDoorTableBuiltInUnknownReturnsNull();
void TestPerTruckDoorTableBuiltInHasMultipleAtsRows();
void TestPerTruckDoorTableNewRowsAreFindable();
void TestApplyOverrideNullIsNoOp();
void TestApplyOverrideDoorPatchIsSentinelAware();

// M2 Game-Free-Camera shim (MVP for the GPU render push).
void TestGameFreeCameraShimDefaultStateIsDisabled();
void TestGameFreeCameraShimSetEnabledRoundTrip();
void TestGameFreeCameraShimVkCustomisation();
void TestGameFreeCameraShimOnEnterWalkingDisabledIsAdvisory();
void TestGameFreeCameraShimOnEnterWalkingSupportedFalseIsAdvisory();
void TestGameFreeCameraShimTriggerToggleRespectsDisabled();
void TestGameFreeCameraShimEchoSuppressionAfterTrigger();
// BindGameHwnd + PostMessage dispatch path — fixes the ATS 1.61.3.1s
// dev-free-camera regression where SendInput did not reach the game
// window in fullscreen / DXGI exclusive mode.
void TestGameFreeCameraShimDefaultHwndIsNull();
void TestGameFreeCameraShimBindGameHwndRoundTrip();
void TestGameFreeCameraShimBindGameHwndOverwrites();
void TestGameFreeCameraShimBindGameHwndNullClears();
void TestGameFreeCameraShimDispatchSkipsWithoutHwnd();
void TestGameFreeCameraShimDispatchUsesSendInputWhenHwndBound();
void TestGameFreeCameraShimRebindOnEntryCatchesLateWindow();
void TestCameraHijackComputeDispatcherVaZero();
void TestCameraHijackComputeDispatcherVaAtsKbExe();
void TestCameraHijackComputeDispatcherVaEts2();
void TestCameraHijackIsRvaExecutableInText();
void TestCameraHijackIsRvaExecutableAtBoundary();
void TestCameraHijackIsRvaExecutableAtBoundaryJustAbove();
void TestCameraHijackIsRvaExecutableInRdata();
void TestCameraHijackIsRvaExecutableOutOfAllSections();
void TestCameraHijackIsRvaExecutableEmptyTable();
void TestCameraHijackDispatcherRvaInText();
void TestCameraHijackComputeSingletonGlobalVaZero();
void TestCameraHijackComputeSingletonGlobalVaAtsKbExe();
void TestCameraHijackComputeVtableSlot16VaZero();
void TestCameraHijackComputeVtableSlot16VaAtsKbExe();
void TestGameFreeCameraShimOnExitWalkingLeavesCountersIntact();
void TestGameFreeCameraShimOnExitWalkingDispatchesWhenSupported();
void TestGameFreeCameraShimOnExitWalkingHonoursDisabledGate();
void TestGameFreeCameraShimOnExitWalkingHonoursUnsupportedGate();
void TestGameFreeCameraShimEntryAndExitToggleParity();
void TestGameFreeCameraShimProbeConfigCandidatesCoversOneDrive();
void TestGameFreeCameraShimProbeConfigCandidatesATSFirst();
void TestGameFreeCameraShimReadGDeveloperValueEmptyOnEmptyPath();
void TestGameFreeCameraShimReadGDeveloperValueParsesOne();
void TestGameFreeCameraShimReadGDeveloperValueParsesZero();
void TestGameFreeCameraShimReadGDeveloperValueEmptyOnMissingKey();
void TestGameFreeCameraShimReadGDeveloperValueMalformedNoQuotes();
void TestGameFreeCameraShimReadGDeveloperValueStopsAtFirstLine();
void TestGameFreeCameraShimReadControlsSiiKeyEmptyOnEmptyPath();
void TestGameFreeCameraShimReadControlsSiiKeyFindsKeyboardBinding();
void TestGameFreeCameraShimReadControlsSiiKeyFindsAlternateName();
void TestGameFreeCameraShimReadControlsSiiKeyFindsCamdbg();
void TestGameFreeCameraShimReadControlsSiiKeyPicksKeyboardAmongMany();
void TestGameFreeCameraShimReadControlsSiiKeyEmptyOnMissingAction();
void TestGameFreeCameraShimReadControlsSiiKeyOnlyJoystickBinding();
void TestGameFreeCameraShimReadControlsSiiKeyHandlesMalformedNoBacktick();
void TestGameFreeCameraShimReadControlsSiiKeyDiagPlainFormNoTypo();
void TestGameFreeCameraShimReadControlsSiiKeyDiagEscapedFormSetsTypo();
void TestGameFreeCameraShimReadControlsSiiKeyDiagEscapedFormNoKeyboard();
void TestGameFreeCameraShimReadControlsSiiKeyDiagMissingActionNoTypo();
void TestGameFreeCameraShimReadControlsSiiKeyDiagNullOutParam();
void TestGameFreeCameraShimReadControlsSiiKeyDiagEscapedWalkMode();
void TestGameFreeCameraShimScsKeyNameToVkEmptyIsZero();
void TestGameFreeCameraShimScsKeyNameToVkWrongPrefixIsZero();
void TestGameFreeCameraShimScsKeyNameToVkFunctionKeys();
void TestGameFreeCameraShimScsKeyNameToVkNumpad();
void TestGameFreeCameraShimScsKeyNameToVkModifiersAndSpecial();
void TestGameFreeCameraShimScsKeyNameToVkCursorAndLetters();
void TestGameFreeCameraShimScsKeyNameToVkUnknownTokenIsZero();
void TestGameFreeCameraShimProbeControlsSiiCandidatesCoversOneDrive();
void TestGameFreeCameraShimAutoDetectAdoptsBindingWhenFound();
void TestGameFreeCameraShimAutoDetectNoBindingLeavesDefaultVk();
void TestGameFreeCameraShimAutoDetectInitialFieldsAreEmpty();
void TestGameFreeCameraShimAutoEnableRequiresIniNotExplicit();
void TestGameFreeCameraShimAutoEnableRequiresBinding();
void TestGameFreeCameraShimAutoEnableRequiresSupported();
void TestGameFreeCameraShimAutoEnableIsIdempotent();
void TestPxSceneHookCatalogConstantsMatchAmtrucksLayout();
void TestDevCameraCvarPatcherDefaultStateIsEnabled();
void TestDevCameraCvarPatcherSetEnabledRoundTrip();
void TestDevCameraCvarPatcherSetNameRvaRoundTrip();
void TestDevCameraCvarPatcherPatchOnceSkipsWhenNoModule();
void TestDevCameraCvarPatcherPatchOnceSkipsWhenDisabled();
void TestDevCameraCvarPatcherPatchOncePatchesFromZeroToOne();
void TestDevCameraCvarPatcherPatchOnceIsIdempotent();
void TestDevCameraCvarPatcherPatchOnceSkipsWhenValueAlreadyOne();
void TestDevCameraCvarPatcherPatchOnceDetectsSignatureMismatch();
void TestDevCameraCvarPatcherCountersAfterSuccessfulPatch();
void TestDevCameraCvarPatcherProbesValueAtStructOffset24();
void TestDevCameraCvarPatcherWiderScanWindowFindsFarStruct();
void TestDevCameraCvarPatcherProbesValueAtStructOffset32();
void TestPxSceneHookFreshInstanceIsZero();
void TestPxSceneHookObserveStoresFirstCapture();
void TestPxSceneHookObserveKeepsFirstCaptureOnLaterNonZeroCalls();
void TestPxSceneHookObserveIgnoresZero();
void TestPxSceneHookResetForTestingClearsState();
void TestPxSceneHookInstallWithoutAmtrucksLeavesTargetUnreachable();
void TestPxSceneHookInstallUninstallRoundtripIsIdempotent();
void TestImGuiOverlayDefaultStateIsDisabled();
void TestImGuiOverlaySetEnabledRoundTrip();
void TestImGuiOverlayOnEnterWalkingIncrementsCounter();
void TestImGuiOverlayOnExitWalkingIncrementsCounter();
void TestImGuiOverlayCountersStayIndependent();
void TestImGuiOverlayResetForTestingIsSafe();
void TestImGuiOverlayVersionStringIsNonNull();
void TestImGuiOverlayInitializeRefusesNullDevice();
void TestImGuiOverlayInitializeIsIdempotent();
void TestImGuiOverlayRenderNoOpWhenDisabled();
void TestImGuiOverlayRenderNoOpWhenSupportedFalse();
void TestImGuiOverlayOnDeviceLostIsSafeWhenUninitialized();
void TestImGuiOverlayBindFrameLoopRoundTrip();
void TestImGuiOverlayButtonClicksStartsAtZero();
void TestImGuiOverlayButtonClicksResetsBetweenOverlays();
void TestImGuiOverlayBindFsmRoundTrip();
void TestImGuiOverlayResetForTestingClearsBindings();
// M11: bind the in-game settings menu so the overlay's render
// path calls SettingsMenu::Render() each frame.
void TestImGuiOverlayBindSettingsMenuRoundTrip();
void TestImGuiOverlayBindSettingsMenuIndependentFromOtherBindings();
void TestApplyOverrideCabinBoxPatch();
void TestTruckDoorConfigAppliesOverride();

void TestLogCustomSinkReceivesAllLevels();
void TestLogLevelReachesSink();
void TestLogShutdownIdempotent();
void TestLogNullSinkIsSafe();

// M8 fuel telemetry sync.
void TestFuelProviderSyncFromTelemetry();
void TestFuelProviderRetainsLocalWhenTelemetryMissing();
void TestFuelProviderFuelingAdvancesAfterTelemetry();
void TestFuelStationReadAfterTickReflectsCurrentPlayerPosition();
// M8 Lander: per-truck tank capacities from the SCS configuration
// event (`fuel.capacity` / `adblue.capacity`).
void TestFuelProviderCapacityFromConfigEventOverridesStub();
void TestFuelProviderFuelingClampsToRealPerTruckCapacity();
void TestFuelProviderStubCapacityHoldsWithoutConfigEvent();
void TestFuelProviderAdblueCapacityFromConfigEventOverridesStub();
void TestDiagnosticDumpSafeBeforeInit();
void TestDiagnosticDumpReportsScsVersion();
void TestDiagnosticDumpMirrorsFsmLabel();
void TestEncodeFsmLabelEveryState();
void TestEncodeFsmLabelProgressOnlyForTransitions();
void TestEncodeFsmLabelClampsProgressToUnitInterval();
void TestEncodeFsmLabelBoundedOutput();
void TestDumpKnockoutHandlesUninitialized();
void TestDumpKnockoutReportsStateAndConfig();
void TestDumpPerTruckDoorHandlesEmpty();
void TestDumpPerTruckDoorReportsMatchedRow();
void TestDumpPerTruckDoorReportsUnmatchedRow();
void TestDumpCouplingHandlesUninitialized();
void TestDumpCouplingReportsActiveStep();
void TestDumpBuildInfoEmitsBannerAndFields();
void TestFsmLabelNeedsReencode();
void TestFsmLabelNeedsReencodeSteadyStateIsFalse();
void TestFsmLabelNeedsReencodeTransitionAlwaysTrue();
void TestFsmLabelCanonicalBuffer();
void TestFsmLabelCacheDecisionDrivesEncodeCalls();
void TestDiagnosticDumpReportsFuelAndAdBlue();
void TestDiagnosticDumpReportsFrameLoopCamera();
void TestDiagnosticDumpReportsInputState();
void TestDiagnosticDumpReportsFuelProvider();
void TestDiagnosticDumpReportsKnockout();
void TestDiagnosticDumpReportsD3D11Hook();
void TestDiagnosticDumpSettingsReportsBeforeInit();
void TestDiagnosticDumpSettingsReportsLiveValues();
void TestDiagnosticDumpReportsPlayerModeName();
void TestPlayerModeNameReturnsNamesForAllValues();

void TestDiagnosticDumpMenuReportsNotInitialized();
void TestDiagnosticDumpMenuReportsClosedClean();
void TestDiagnosticDumpMenuReportsOpenClean();
void TestDiagnosticDumpMenuReportsOpenDirty();
void TestDiagnosticDumpMenuMirrorsClose();
void TestDiagnosticDumpFullStateMenuSection();

void TestDiagnosticDumpHeadTrackerReportsNotInitialized();
void TestDiagnosticDumpHeadTrackerReportsConfigAndState();
void TestDiagnosticDumpHeadTrackerReportsActiveAfterMouseFeed();
void TestDiagnosticDumpHeadTrackerPeekDoesNotPerturb();

void TestDiagnosticDumpShadowReportsNotInitialized();
void TestDiagnosticDumpShadowReportsNoPlayerState();
void TestDiagnosticDumpShadowReportsConfigAndProjection();
void TestDiagnosticDumpShadowSunOnHorizonEdge();

void TestDiagnosticDumpFlashlightReportsNotInitialized();
void TestDiagnosticDumpFlashlightReportsNoCameraState();
void TestDiagnosticDumpFlashlightReportsOffByDefault();
void TestDiagnosticDumpFlashlightReportsConeAfterToggle();
void TestDiagnosticDumpDoorReportsNotInitialized();
void TestDiagnosticDumpDoorReportsClosedIdle();
void TestDiagnosticDumpDoorReportsMidwayAnimation();

void TestDiagnosticDumpVersionReportsNotInitialized();
void TestDiagnosticDumpVersionReportsEts2Match();
void TestDiagnosticDumpVersionReportsAtsMatch();
void TestDiagnosticDumpVersionReportsNoMatchForUnknownBuild();

void TestDiagnosticDumpFsmReportsNotInitialized();
void TestDiagnosticDumpFsmReportsDrivingDefault();
void TestDiagnosticDumpFsmReportsMidTransitionOut();
void TestDiagnosticDumpFsmReportsMidTransitionIn();
void TestDiagnosticDumpInputReportsNotInitialized();
void TestDiagnosticDumpInputReportsEmptyState();
void TestDiagnosticDumpInputReportsPerKeyEdges();
void TestDiagnosticDumpInputQuietKeysAreOmitted();
void TestSettingsMenuOpenClosesDirty();
void TestSettingsMenuSetSliderKnownKeyWritesThrough();
void TestSettingsMenuSetSliderJumpVelocityWritesThrough();
void TestSettingsMenuSetSliderGravityWritesThrough();
void TestSettingsMenuSetSliderMaxStepUpWritesThrough();
void TestSettingsMenuSetSliderMaxSlopeAngleWritesThrough();
void TestSettingsMenuSetSliderNearZWritesThrough();
void TestSettingsMenuSetSliderFarZWritesThrough();
void TestSettingsMenuSetSliderUnknownKeyIsSilent();
void TestSettingsMenuEveryDocumentedSliderWritesThrough();
void TestSettingsMenuEveryDocumentedBoolWritesThrough();
void TestSettingsMenuEveryDocumentedKeyBindingWritesThrough();
void TestSettingsMenuUnknownKeysAreSilentAcrossAllApis();
void TestSettingsMenuSetBoolUseMouseDirties();
void TestSettingsMenuSetBoolUnknownKeyIsSilent();
void TestSettingsMenuSetBoolUseTrackirDirties();
void TestSettingsMenuSetBoolUseOpentrackDirties();
void TestSettingsMenuSetBoolApplyAllPushesToProvider();
void TestSettingsMenuSetKeyBindingKnownKeyWritesThrough();
void TestSettingsMenuSetKeyBindingUnknownKeyIsSilent();
void TestSettingsMenuToggleFlipsIsOpen();
void TestSettingsMenuCloseWithoutDirtyDoesNotWrite();
void TestSettingsMenuCloseWithDirtyWritesToDisk();

// SettingsStore::CheckFileChanged() per-frame contract + perf budget.
// See tests/test_settings_check_file_changed.cpp for the rationale on
// each case.
void TestSettingsCheckFileChangedEmptyPathReturnsFalse();
void TestSettingsCheckFileChangedMissingFileReturnsFalse();
void TestSettingsCheckFileChangedNoChangeReturnsFalse();
void TestSettingsCheckFileChangedReloadsOnMtimeChange();
void TestSettingsCheckFileChangedRapidEditsDetected();
void TestSettingsCheckFileChangedReloadOverwritesSetCurrent();
void TestSettingsCheckFileChangedThrottleFirstCallPolls();
void TestSettingsCheckFileChangedThrottleSkipsRapidCalls();
void TestSettingsCheckFileChangedThrottleResetHookUnblocksNextPoll();
void TestSettingsCheckFileChangedThrottleResetIsTransparentToMtimeCheck();
void TestSettingsCheckFileChangedThrottlePollingIntervalIsExposed();
void TestSettingsCheckFileChangedThrottleSkipPathPerfBudget();
void TestSettingsCheckFileChangedPollPathPerfBudget();
void TestSettingsStoreApplyAllPerfBudget();
void TestWalkerFsmUpdateSteadyStatePerfBudget();
void TestWalkerFsmUpdateMidTransitionPerfBudget();

// ScsTelemetryProvider direct probes — the only provider module that
// had no dedicated test file before this commit.
void TestScsTelemetryProviderFreshStateAllHasFlagsAreFalse();
void TestScsTelemetryProviderFreshStateScalarFieldsAreZero();
void TestScsTelemetryProviderStateIsStableAcrossConstAccess();
void TestScsTelemetryProviderRegisterAllNullIsSafe();
void TestScsTelemetryProviderUnregisterAllNullIsSafe();
void TestD3D11HookFreshInstanceIsNotInstalled();
void TestD3D11HookFreshSwapChainGenerationIsZero();
void TestD3D11HookBindFrameLoopNullIsSafe();
void TestD3D11HookBindFrameLoopStoresPointer();
void TestD3D11HookUninstallWithoutInstallIsSafe();
void TestD3D11HookInstallUninstallRoundtrip();
void TestD3D11HookSecondInstallReturnsFalse();
void TestD3D11HookDestructorOnFreshInstanceIsSafe();
void TestD3D11HookDestructorAfterUninstallIsSafe();
void TestD3D12HookFreshInstanceIsNotInstalled();
void TestD3D12HookFreshSwapChainGenerationIsZero();
void TestD3D12HookBindFrameLoopNullIsSafe();
void TestD3D12HookBindFrameLoopStoresPointer();
void TestD3D12HookUninstallWithoutInstallIsSafe();
void TestD3D12HookInstallUninstallRoundtrip();
void TestD3D12HookSecondInstallReturnsFalse();
void TestD3D12HookDestructorOnFreshInstanceIsSafe();
void TestD3D12HookDestructorAfterUninstallIsSafe();
void TestD3D11AndD3D12GenerationCountersAreIndependent();
void TestD3D12HookInstallStateSurvivesSecondUninstall();
void TestD3D11AndD3D12ShareProcessWideMinHook();

// PerFrameThread fallback lander — the background ticker that drives
// walker_per_frame_tick at ~60 Hz when the D3D Present hooks cannot
// (because the game's swap chain was created before WalkerCore loaded).
// These probes pin the lifecycle (Start/Stop idempotency), the
// tick-rate order of magnitude, the per-frame lock serialization
// against a synthetic second caller, and the live counter contract
// that the diagnostic layer will surface when the F12 dump runs.
void TestPerFrameThreadStartStopIsIdempotent();
void TestPerFrameThreadTicksAdvanceOverWindow();
void TestPerFrameTickLockSerializesTwoCallers();
void TestPerFrameThreadSurvivesManyStartStopCycles();

// Time.h contract probes — NowMicros monotonicity / defensive
// fallback / realistic delta, plus DeltaSeconds positive / zero /
// clamped / microsecond-precision. The Time module had only
// indirect coverage (via test_settings_check_file_changed.cpp and
// test_frame_loop_perf.cpp perf budgets) before this commit.
void TestTimeNowMicrosReturnsNonZero();
void TestTimeNowMicrosIsMonotonic();
void TestTimeNowMicrosAdvancesOverSleep();
void TestTimeDeltaSecondsPositive();
void TestTimeDeltaSecondsZeroWhenEqual();
void TestTimeDeltaSecondsClampedWhenNegative();
void TestTimeDeltaSecondsHandlesRealisticDelta();
void TestTimeDeltaSecondsMicrosecondPrecision();

}  // namespace walkercore

int main() {
    fprintf(stderr, "WalkerCore unit tests:\n");

    RUN_TEST(walkercore::TestCameraViewMatrix);
    RUN_TEST(walkercore::TestCameraProjectionMatrix);
    RUN_TEST(walkercore::TestCameraAdoptPlayer);
    RUN_TEST(walkercore::TestCameraStateUpdateEveryFieldHasObservableEffect);
    RUN_TEST(walkercore::TestCameraStateUpdatePerfBudget);
    RUN_TEST(walkercore::TestCameraStateProjectionCacheHitWhenInputsUnchanged);
    RUN_TEST(walkercore::TestCameraStateProjectionCacheInvalidatedByEachInput);
    RUN_TEST(walkercore::TestCameraStateUpdateCachedPathPerfBudget);
    // View-rotation cache probes — split BuildViewMatrix into rotation
    // block + translation row so Cache hit / miss / dirty-flag paths
    // are pinned the same way as the projection cache.
    RUN_TEST(walkercore::TestCameraStateViewRotationCacheHitWhenInputsUnchanged);
    RUN_TEST(walkercore::TestCameraStateViewRotationCacheInvalidatedByYaw);
    RUN_TEST(walkercore::TestCameraStateViewRotationCacheInvalidatedByPitch);
    RUN_TEST(walkercore::TestCameraStateViewRotationCacheNegativeControlOnEyePosition);
    RUN_TEST(walkercore::TestCameraStateViewRotationCacheExplicitDirtyFlag);
    RUN_TEST(walkercore::TestCameraStateViewRotationCacheFullBuildMatchesSplit);
    RUN_TEST(walkercore::TestCameraStateUpdateFullCacheHitPerfBudget);

    RUN_TEST(walkercore::TestMovementConfigDefaults);
    RUN_TEST(walkercore::TestMovementConfigEveryFieldHasObservableEffect);
    RUN_TEST(walkercore::TestTruckLocalRoundtrip);
    RUN_TEST(walkercore::TestTruckLocalWorldIdentity);
    RUN_TEST(walkercore::TestTruckLocalTransformEveryFieldHasObservableEffect);

    RUN_TEST(walkercore::TestIniParserBasic);
    RUN_TEST(walkercore::TestIniParserKeys);
    RUN_TEST(walkercore::TestIniParserComments);
    RUN_TEST(walkercore::TestIniParserUnknownKeys);
    RUN_TEST(walkercore::TestIniRoundtrip);
    RUN_TEST(walkercore::TestIniHeadTrackerSectionParsed);
    RUN_TEST(walkercore::TestIniHeadTrackerUnknownKeyIgnored);

    // M2 lander INI gates — defaults = false, parse `true` roundtrips
    // through Save+Load (then the auto-migration forces to false),
    // a typo key (e.g. `d3d_11_hook`) does NOT silently flip the
    // gate, and a pre-existing INI with the OLD `true` default is
    // auto-migrated to `false` on the next load. These are the
    // safety net for the ATS 1.61.3.1s + ReShade + OptiScaler +
    // RenoDX crash on amtrucks.exe+0x1d5300a (movdqa).
    RUN_TEST(walkercore::TestIniM2D3DHookGatesParseAndDefault);
    RUN_TEST(walkercore::TestIniM2D3DHookGatesUnknownKeyIgnored);
    RUN_TEST(walkercore::TestIniM2D3DHookGatesAutoMigratesOldTrueValues);
    // M2 camera_hijack_install + M3 px_scene_hook_install gates —
    // same crash rationale as the d3d11/d3d12 gates, but for the
    // in-process MinHook trampolines on amtrucks.exe.
    RUN_TEST(walkercore::TestIniM2CameraHijackInstallDefaultIsFalse);
    RUN_TEST(walkercore::TestIniM3PxSceneHookInstallDefaultIsFalse);
    RUN_TEST(walkercore::TestIniM2CameraHijackInstallParseAndRoundTrip);
    RUN_TEST(walkercore::TestIniM3PxSceneHookInstallParseAndRoundTrip);
    RUN_TEST(walkercore::TestIniM2CameraHijackInstallAutoMigratesOldTrueValues);
    RUN_TEST(walkercore::TestIniM3PxSceneHookInstallAutoMigratesOldTrueValues);
    RUN_TEST(walkercore::TestIniM2GameFreeCameraShimDefaultIsFalse);
    RUN_TEST(walkercore::TestIniM2GameFreeCameraShimAutoMigratesOldTrueToFalse);
    RUN_TEST(walkercore::TestIniM2GameFreeCameraShimExplicitFalseIsPreserved);
    // M2 dev_camera_toggle_vk.
    RUN_TEST(walkercore::TestIniM2DevCameraToggleVkDefaultIsVKF11);
    RUN_TEST(walkercore::TestIniM2DevCameraToggleVkParsesHexAndDecimal);
    RUN_TEST(walkercore::TestIniM2DevCameraToggleVkOutOfRangeKeepsDefault);
    RUN_TEST(walkercore::TestIniM2DevCameraToggleVkUnparseableKeepsDefault);
    RUN_TEST(walkercore::TestIniM2DevCameraToggleVkRoundTripsThroughSaveToPath);
    RUN_TEST(walkercore::TestIniM2SentinelSuppressesExplicitFlagForAutoWrittenFalse);
    RUN_TEST(walkercore::TestIniM2NoSentinelMeansExplicitTrue);
    RUN_TEST(walkercore::TestIniM2SentinelForDevCameraToggleVk);
    RUN_TEST(walkercore::TestIniSaveToPathEmitsSentinelForDefaultValues);
    RUN_TEST(walkercore::TestIniSaveToPathOmitsSentinelForNonDefaultValues);
    RUN_TEST(walkercore::TestIniM2LegacyIniGetsSentinelPatchedInPlace);
    RUN_TEST(walkercore::TestIniM2LegacyPatchSkipsNonDefaultValues);

    // Regression guard for the dump_state_vk / mouse-button round-trip —
    // the user observed "F12 only gives me a screenshot" because the
    // dump key collided with the in-game screenshot binding and the
    // INI never exposed a way to override it.
    RUN_TEST(walkercore::TestIniDumpStateVkRoundTrip);
    RUN_TEST(walkercore::TestIniSaveEmitsDumpStateVkAndMouseButtons);
    RUN_TEST(walkercore::TestIniMissingKeysDefaultIsPreserved);

    // SettingsStore::ApplyAll() push contract — every leaf field must
    // reach its bound consumer.
    RUN_TEST(walkercore::TestApplyAllPushesEveryFieldToEveryConsumer);

    RUN_TEST(walkercore::TestWalkerFsmInitialState);
    RUN_TEST(walkercore::TestWalkerFsmDrivingToWalking);
    RUN_TEST(walkercore::TestWalkerFsmWalkingToDriving);
    RUN_TEST(walkercore::TestWalkerFsmF10FromWalkingExits);
    RUN_TEST(walkercore::TestWalkerFsmBindPlayerTarget);
    RUN_TEST(walkercore::TestWalkerFsmNoPlayerTargetIsSafe);
    RUN_TEST(walkercore::TestWalkerFsmShouldRenderOverridePerState);
    RUN_TEST(walkercore::TestWalkerFsmTransitionProgressMonotonic);
    RUN_TEST(walkercore::TestWalkerFsmInteractOutOfRangeIsSilent);
    RUN_TEST(walkercore::TestWalkerFsmStateNameReturnsNameForEveryValue);
    RUN_TEST(walkercore::TestWalkerFsmStateNamesAreDistinct);
    RUN_TEST(walkercore::TestWalkerFsmInputsEveryFieldHasObservableEffect);
    RUN_TEST(walkercore::TestWalkerFsmTransitionTargetMath);

    RUN_TEST(walkercore::TestFrameLoopIntegration);
    RUN_TEST(walkercore::TestFrameLoopCameraOverride);
    RUN_TEST(walkercore::TestFrameLoopNoFsmIsNoop);

    // PhaseIntegrateMovement motion math probes: diagonal speed-bounding
    // and crouch-vs-run modifier priority. These are real regression
    // shibboleths — a future refactor that breaks either would be
    // user-visible on the very first walk.
    RUN_TEST(walkercore::TestFrameLoopDiagonalMotionIsSpeedBounded);
    RUN_TEST(walkercore::TestFrameLoopCrouchWinsOverRun);

    // PhaseAnimatePlayer head-bob probes — pin the not-moving
    // short-circuit (sin/cos are skipped when the player isn't in a
    // moving mode), the moving branch still applies the bob, the
    // stationary decay shrinks the amplitude monotonically, and a
    // zero-amplitude config produces zero bob contribution.
    RUN_TEST(walkercore::TestPhaseAnimatePlayerSkipsSinCosWhenNotMoving);
    RUN_TEST(walkercore::TestPhaseAnimatePlayerBobAppliesWhenMoving);
    RUN_TEST(walkercore::TestPhaseAnimatePlayerBobDecaysWhenStationary);
    RUN_TEST(walkercore::TestPhaseAnimatePlayerZeroAmplitudeIsSilent);
    RUN_TEST(walkercore::TestFrameLoopSetAspectIsNoopWhenUnchanged);
    RUN_TEST(walkercore::TestPhaseAnimatePlayerKeepsHeadBobPhaseBounded);
    RUN_TEST(walkercore::TestPhaseIntegrateMovementSkipsSqrtWhenIntentIsOnAxis);
    RUN_TEST(walkercore::TestPhaseIntegrateMovementClampsVerticalVelocity);
    RUN_TEST(walkercore::TestMovementConfigVerticalVelocityCapDefault);

    // Floor Y-clamp probes — see FrameLoop.h SetFloorY for context.
    RUN_TEST(walkercore::TestFrameLoopFloorClampDefaultsToZero);
    RUN_TEST(walkercore::TestFrameLoopFloorClampSnapsPlayerUp);
    RUN_TEST(walkercore::TestFrameLoopFloorClampMarksGrounded);
    RUN_TEST(walkercore::TestFrameLoopFloorClampIgnoresPlayerAboveFloor);
    RUN_TEST(walkercore::TestFrameLoopFloorClampHandlesFallingPlayer);

    // Per-frame conductor input-sample contract. Pin: RunOnce must not
    // wipe the rising edge the conductor's SampleInput just produced
    // (otherwise the fuel / coupling / flashlight providers see the F
    // press on the wrong frame). Also pin: RunOnce still samples when
    // called directly without a preceding SampleInput (so the test
    // harness / non-conductor drivers keep working).
    RUN_TEST(walkercore::TestFrameLoopRunOnceDoesNotClobberConductorInput);
    RUN_TEST(walkercore::TestFrameLoopRunOnceSamplesWhenConductorDidNot);

    // Per-frame conductor snapshot-consistency contract. The conductor
    // caches g_frame_loop->player() into a pointer at the top of
    // walker_per_frame_tick; the FSM, knockout, fuel, and coupling
    // phases all observe the same live player storage. This pins the
    // "same snapshot on all consumers" contract by verifying the
    // FrameLoop's player_ member aliases the same memory the FSM's
    // BindPlayerTarget wires in.
    RUN_TEST(walkercore::TestFrameLoopPlayerSnapshotStableAcrossConductor);

    // Override-path field-by-field copy contract. The override path
    // (camera_ = camera_override_) used to wholesale-copy the entire
    // CameraState struct, which clobbered camera_.aspect with whatever
    // bytes the FSM's stack-allocated `CameraState target;` had on
    // the previous frame — breaking the projection matrix on every
    // override frame. The new path copies only eye/yaw/pitch (the
    // fields the FSM writes) and explicitly clears the precomputed-
    // yaw-trig cache so BuildViewRotation recomputes fresh on the
    // override frame. Pin both contracts.
    RUN_TEST(walkercore::TestFrameLoopOverridePathPreservesProjectionFields);
    RUN_TEST(walkercore::TestFrameLoopWalkingPopulatesViewAndProjectionMatrices);
    RUN_TEST(walkercore::TestFrameLoopHeadTrackerNotInitialisedByDefault);
    RUN_TEST(walkercore::TestFrameLoopInitHeadTrackerIsIdempotent);
    RUN_TEST(walkercore::TestFrameLoopHeadTrackerShutdownRestoresBaseline);
    RUN_TEST(walkercore::TestFrameLoopHeadTrackerTickIsNoOpWithoutReceiver);
    RUN_TEST(walkercore::TestFrameLoopHeadTrackerStateDefaultSourceIsSentinel);

    // M7 lander math — separate mouse vs head-tracker accumulators.
    // The lander splits yaw/pitch into two streams (player body +
    // camera-relative head offset) and sums them in Phase 6. These
    // tests pin the split: head tracker never touches player.yaw,
    // Phase 6 adds the offset to camera.yaw, the cache is invalidated
    // when the offset is applied, the offset returns to 0 on
    // disconnect, and the post-add pitch is clamped to ±89°.
    RUN_TEST(walkercore::TestM7LanderMath_MouseOnlyPathKeepsCameraEqualToPlayer);
    RUN_TEST(walkercore::TestM7LanderMath_HeadTrackerDoesNotMutatePlayerYaw);
    RUN_TEST(walkercore::TestM7LanderMath_HeadTrackerOffsetAddsToCameraYaw);
    RUN_TEST(walkercore::TestM7LanderMath_HeadTrackerOffsetAddsToCameraPitch);
    RUN_TEST(walkercore::TestM7LanderMath_PitchClampedWhenOffsetExceedsLimitMax);
    RUN_TEST(walkercore::TestM7LanderMath_ShutdownReturnsCameraToBodyYaw);
    RUN_TEST(walkercore::TestM7LanderMath_TrigCacheInvalidatedWhenOffsetAdded);

    // WalkerFsmInputs pointer-refactor contract. The per-frame
    // conductor uses UpdateFromPointers to avoid a 152-byte/frame
    // copy. This pins: (1) struct size is small, (2) the two
    // call paths produce equivalent state transitions, (3) mutations
    // to the source player are observable on the next Update.
    RUN_TEST(walkercore::TestWalkerFsmInputsPointerRefactorContract);
    RUN_TEST(walkercore::TestWalkerFsmTransitionOutCameraFromMatchesCabinSeatWhenPointerAliases);

    // CameraState view-rotation sqrt-skip contract. When pitch == 0
    // (looking horizontally, the most common case in Walking), the
    // right vector is already unit length and the sqrtf+divide to
    // "normalise" it is pure waste. This pins the skip's correctness
    // across yaw = 0, π/2, π/4, and asserts the non-zero-pitch path
    // still matches the explicit formula.
    RUN_TEST(walkercore::TestCameraStateBuildViewRotationPitchZeroSkipsNormalisation);

    // CameraState precomputed sin(yaw)/cos(yaw) cache. PhaseApplyMouseLook
    // writes the cache once per RunOnce; PhaseIntegrateMovement reads it
    // for ground-plane motion; CameraState_AdoptPlayer forwards it into
    // camera_.precomputed_yaw_trig_* so BuildViewRotation's cache-miss
    // path also skips the trig. This pins (1) the cached and plain paths
    // produce identical rotation blocks, (2) yaw=0 (sin=0) does NOT
    // disable the cache via a false-zero check, (3) CameraState_Reset
    // clears the flag (the override path's safety), (4) the invalid-cache
    // path still produces a correct matrix.
    RUN_TEST(walkercore::TestCameraStatePrecomputedYawTrigCacheContract);

    // CameraState_Reset must clear the precomputed-yaw-trig cache flag
    // and zero out the sin/cos fields. The FrameLoop constructor calls
    // CameraState_Reset on its camera_override_ field at init time so a
    // stale valid-bit can't leak into the override path's first frame.
    // Without this reset, the camera_override_'s precomputed_yaw_trig_valid_
    // would be uninitialized (CameraState has no explicit constructor),
    // and a future refactor that fires OverrideCamera before any
    // CameraState_Update could feed garbage sin/cos into BuildViewRotation.
    RUN_TEST(walkercore::TestCameraStateResetClearsPrecomputedYawTrigCache);

    // Per-frame performance budget tests — catch hot-path regressions
    // in FrameLoop + WalkerFsm + InputHook update chain. Generous 200
    // µs/tick ceiling; real cost on modern x86 is well under 50 µs.
    RUN_TEST(walkercore::TestFrameLoopRunOnceHotPathPerfBudget);
    RUN_TEST(walkercore::TestPerFrameTickConductorPerfBudget);
    RUN_TEST(walkercore::TestInputHookSamplePerfBudget);

    RUN_TEST(walkercore::TestKnockoutThreshold);
    RUN_TEST(walkercore::TestKnockoutRunningDoesNotTrigger);
    RUN_TEST(walkercore::TestKnockoutFastImpactTriggers);
    RUN_TEST(walkercore::TestKnockoutProviderStateCycleAndEveryField);
    RUN_TEST(walkercore::TestKnockoutTeleportReprime);
    RUN_TEST(walkercore::TestKnockoutRecoveryResetsLastPositionBaseline);
    RUN_TEST(walkercore::TestKnockoutPrimeResetsTrackingBaseline);
    RUN_TEST(walkercore::TestKnockoutConfigDefaultValues);
    RUN_TEST(walkercore::TestKnockoutPathologicalDtGuards);
    // M10 Lander: ||player.velocity|| as the trigger metric.
    RUN_TEST(walkercore::TestKnockoutTriggerUsesIntegratedVelocityNotDistDt);
    RUN_TEST(walkercore::TestKnockoutTriggerFiresOnIntegratedVelocityOnly);
    RUN_TEST(walkercore::TestKnockoutTriggerUsesMagnitudeAcrossAxes);
    RUN_TEST(walkercore::TestKnockoutStationaryPlayerNeverTriggers);
    RUN_TEST(walkercore::TestDoorAnimEasing);
    RUN_TEST(walkercore::TestDoorAnimProviderEveryFieldHasObservableEffect);
    RUN_TEST(walkercore::TestDoorAnimWiringInitialStateNotAnimating);
    RUN_TEST(walkercore::TestDoorAnimWiringDriveToExitOpensDoor);
    RUN_TEST(walkercore::TestDoorAnimWiringWalkToEnterClosesDoor);
    RUN_TEST(walkercore::TestDoorAnimWiringDoorTweenCompletes);
    RUN_TEST(walkercore::TestDoorAnimWiringTickAdvancesRegardlessOfState);
    RUN_TEST(walkercore::TestDoorAnimWiringStopCancelsTween);
    RUN_TEST(walkercore::TestDoorAnimWiringDumpReportsNotInitialized);
    RUN_TEST(walkercore::TestDoorAnimWiringDumpReportsIdleAngle);
    RUN_TEST(walkercore::TestDoorAnimWiringDumpReportsAnimating);
    RUN_TEST(walkercore::TestDoorAnimWiringTransitionStartToAngleAtTarget);

    RUN_TEST(walkercore::TestInputHookRisingEdgeVisible);
    RUN_TEST(walkercore::TestInputHookRisingEdgeConsumedAfterSample);
    RUN_TEST(walkercore::TestInputHookFallingEdgeVisible);
    RUN_TEST(walkercore::TestInputHookKeyRepeatNoExtraEdges);
    RUN_TEST(walkercore::TestInputHookMultipleKeysIndependent);
    RUN_TEST(walkercore::TestInputHookDumpStateKeyDefaultIsF12);
    RUN_TEST(walkercore::TestInputHookDumpStateRisingEdgeConsumed);
    RUN_TEST(walkercore::TestInputHookDumpStateFallingEdgeVisible);
    RUN_TEST(walkercore::TestInputHookClassifyToggleWalkAlone);
    RUN_TEST(walkercore::TestInputHookClassifyToggleWalkWithCrouchHeld);

    // WalkercoreKeyName probe — same pattern as Feature /
    // WalkerFsmState / HeadTrackerSource probes.
    RUN_TEST(walkercore::TestWalkercoreKeyNameReturnsNameForEveryValue);
    RUN_TEST(walkercore::TestWalkercoreKeyRangeIsContiguous);

    // M4 MousePump — RAWINPUT-driven motion / wheel / button
    // accumulation. Production path is OnRawInputMessage(LPARAM); the
    // tests drive OnRawInputData(const RAWINPUT&) directly.
    RUN_TEST(walkercore::TestInputHookOnRawInputMotionAccumulates);
    RUN_TEST(walkercore::TestInputHookOnRawInputMotionDrainsOnNextSample);
    RUN_TEST(walkercore::TestInputHookOnRawInputAbsoluteMotionIsDropped);
    RUN_TEST(walkercore::TestInputHookOnRawInputWheelAccumulates);
    RUN_TEST(walkercore::TestInputHookOnRawInputWheelDrainsOnNextSample);
    RUN_TEST(walkercore::TestInputHookOnRawInputLeftButtonPress);
    RUN_TEST(walkercore::TestInputHookOnRawInputLeftButtonRelease);
    RUN_TEST(walkercore::TestInputHookOnRawInputRightButtonPress);
    RUN_TEST(walkercore::TestInputHookOnRawInputButtonHeldNoExtraEdges);
    RUN_TEST(walkercore::TestInputHookOnRawInputIgnoresNonMouse);
    RUN_TEST(walkercore::TestInputHookOnRawInputCombinedMotionAndButton);

    // M11.4 — WndProc subclass. The fixture creates a fresh
    // message-only window per test (HWND_MESSAGE parent), so the
    // tests don't depend on the test process's window station or
    // the desktop being available.
    RUN_TEST(walkercore::TestWindowSubclassFreshStateIsNotInstalled);
    RUN_TEST(walkercore::TestWindowSubclassInstallOnNullHwndIsRefused);
    RUN_TEST(walkercore::TestWindowSubclassInstallUninstallRoundTrip);
    RUN_TEST(walkercore::TestWindowSubclassInstallIsIdempotent);
    RUN_TEST(walkercore::TestWindowSubclassUninstallWhenNotInstalledIsSafe);
    RUN_TEST(walkercore::TestWindowSubclassUninstallWithMismatchedHwndIsIgnored);
    RUN_TEST(walkercore::TestWindowSubclassReinstallReplacesHwnd);
    RUN_TEST(walkercore::TestWindowSubclassRawInputSinkReceivesWM_INPUT);
    RUN_TEST(walkercore::TestWindowSubclassRawInputSinkReceivesMultipleWM_INPUT);
    RUN_TEST(walkercore::TestWindowSubclassClearedSinkDropsWM_INPUT);
    RUN_TEST(walkercore::TestWindowSubclassSinkSurvivesReinstall);
    RUN_TEST(walkercore::TestWindowSubclassChainsToOriginalForNonWM_INPUT);
    RUN_TEST(walkercore::TestWindowSubclassChainsToOriginalForMultipleMessages);
    RUN_TEST(walkercore::TestWindowSubclassWM_INPUTAlsoChainsToOriginal);
    RUN_TEST(walkercore::TestWindowSubclassRestoresOriginalOnUninstall);

    RUN_TEST(walkercore::TestHeadTrackerSteadyStateConverges);
    RUN_TEST(walkercore::TestHeadTrackerNoInputDoesNotDrift);
    RUN_TEST(walkercore::TestHeadTrackerNegativeDeltaReturns);
    RUN_TEST(walkercore::TestHeadTrackerDisabledSourceIgnored);
    RUN_TEST(walkercore::TestHeadTrackerSourceNameReturnsNameForEveryValue);
    RUN_TEST(walkercore::TestHeadTrackerSourceRangeIsContiguous);
    RUN_TEST(walkercore::TestHeadTrackerProviderEveryFieldHasObservableEffect);

    // M7 head tracker (wiring + dump).
    RUN_TEST(walkercore::TestHeadTrackerWiringDefaultConfigAllDisabled);
    RUN_TEST(walkercore::TestHeadTrackerWiringFreshProviderReportsInactive);
    RUN_TEST(walkercore::TestHeadTrackerWiringInstallIdempotent);
    RUN_TEST(walkercore::TestHeadTrackerWiringSourceNameAllValues);
    RUN_TEST(walkercore::TestHeadTrackerWiringDumpReportsNotInitialized);
    RUN_TEST(walkercore::TestHeadTrackerWiringDumpReportsInactiveByDefault);
    RUN_TEST(walkercore::TestHeadTrackerWiringDumpReportsActiveAfterMouseFeed);
    RUN_TEST(walkercore::TestHeadTrackerWiringDumpReportsYawAfterMouseFeed);
    RUN_TEST(walkercore::TestHeadTrackerWiringDumpUpdatesOnFeed);
    RUN_TEST(walkercore::TestHeadTrackerWiringConfigOverride);
    RUN_TEST(walkercore::TestHeadTrackerWiringPeekDoesNotAdvance);
    RUN_TEST(walkercore::TestHeadTrackerWiringPeekIsConst);

    // M7 shadow provider (wiring + dump).
    RUN_TEST(walkercore::TestShadowWiringDefaultSunDirection);
    RUN_TEST(walkercore::TestShadowWiringComputeActiveWithFeet);
    RUN_TEST(walkercore::TestShadowWiringComputeInactiveWithoutFeet);
    RUN_TEST(walkercore::TestShadowWiringComputeRadiusIsPositive);
    RUN_TEST(walkercore::TestShadowWiringSetConfigAppliesImmediately);
    RUN_TEST(walkercore::TestShadowWiringSunOnHorizonIsSafe);
    RUN_TEST(walkercore::TestShadowWiringDumpReportsNotInitialized);
    RUN_TEST(walkercore::TestShadowWiringDumpReportsNoPlayerState);
    RUN_TEST(walkercore::TestShadowWiringDumpReportsActiveWithFrameLoopAndPlayer);
    RUN_TEST(walkercore::TestShadowProviderComputeEveryFieldHasObservableEffect);

    // M7 flashlight (state + dump wiring).
    RUN_TEST(walkercore::TestFlashlightInitialStateOff);
    RUN_TEST(walkercore::TestFlashlightToggleFlips);
    RUN_TEST(walkercore::TestFlashlightSetOnExplicit);
    RUN_TEST(walkercore::TestFlashlightOffHasZeroIntensity);
    RUN_TEST(walkercore::TestFlashlightOnHasUnitIntensity);
    RUN_TEST(walkercore::TestFlashlightComputeOriginMatchesCamera);
    RUN_TEST(walkercore::TestFlashlightComputeDirectionMatchesCameraForward);
    RUN_TEST(walkercore::TestFlashlightDirectionIsNormalised);
    RUN_TEST(walkercore::TestFlashlightConeAnglesArePositive);
    RUN_TEST(walkercore::TestFlashlightDumpReportsNotInitializedWhenUnwired);
    RUN_TEST(walkercore::TestFlashlightDumpReportsOffWhenProviderOff);
    RUN_TEST(walkercore::TestFlashlightDumpReportsOnWhenProviderOn);
    RUN_TEST(walkercore::TestFlashlightDumpMirrorsToggle);

    // Architectural SDK-free invariants — pin the rule that domain
    // headers do not include scssdk.h.
    RUN_TEST(walkercore::TestGameIdentityFromFields);
    RUN_TEST(walkercore::TestGameIdentityFromFieldsAts);
    RUN_TEST(walkercore::TestGameIdentityFromFieldsNullGameId);
    RUN_TEST(walkercore::TestGameIdentityEquality);
    RUN_TEST(walkercore::TestGameIdentityMajorMinorHighBits);

    // VersionTable feature coverage probes.
    RUN_TEST(walkercore::TestFeatureNameReturnsNameForEveryValue);
    RUN_TEST(walkercore::TestFeatureBitsAreUniquePowersOfTwo);
    RUN_TEST(walkercore::TestFeatureProbeTableHasExpectedCount);

    // VersionTable build-coverage probes — pin that every documented
    // game build (incl. the Steam-shipped ATS 1.61.3.1s the user
    // reported) resolves to a VersionEntry with the right label.
    RUN_TEST(walkercore::TestAtS1_61_3_1sLookupMatchesSteamBuild);
    RUN_TEST(walkercore::TestVersionTableUnknownBuildReturnsCatchAll);
    RUN_TEST(walkercore::TestVersionTableEveryDocumentedBuildResolves);

    // ABI contract: the 14 public symbols are loadable + callable.
    RUN_TEST(walkercore::TestAbiContractDllLoads);

    // Honest-stub one-shot warning probes.
    RUN_TEST(walkercore::TestPhysXStubWarnsExactlyOnce);
    RUN_TEST(walkercore::TestKnockoutStubWarnsExactlyOnce);
    RUN_TEST(walkercore::TestCouplingStubWarnsExactlyOnce);
    RUN_TEST(walkercore::TestPolishStubWarnsExactlyOncePerName);
    RUN_TEST(walkercore::TestFuelPumpStubWarnsExactlyOnce);

    // M3 PxScene vtable memory-scan probes.
    RUN_TEST(walkercore::TestPhysXMemoryScanSyntheticVtable);
    RUN_TEST(walkercore::TestPhysXMemoryScanRejectsShortBuffer);
    RUN_TEST(walkercore::TestPhysXMemoryScanRejectsNullBuffer);
    RUN_TEST(walkercore::TestPhysXMemoryScanRejectsAllZeroBuffer);
    // M3 multi-strategy detection: strings + signature combined.
    RUN_TEST(walkercore::TestPhysXStringsProbeFindsKnownString);
    RUN_TEST(walkercore::TestPhysXStringsProbeFindsAllKnownStrings);
    RUN_TEST(walkercore::TestPhysXStringsProbeRejectsEmptyBuffer);
    RUN_TEST(walkercore::TestPhysXStringsProbeIgnoresPartialMatch);
    RUN_TEST(walkercore::TestPhysXSignatureRunAcceptedAtThreshold);
    RUN_TEST(walkercore::TestPhysXSignatureRunRejectedBelowThreshold);
    RUN_TEST(walkercore::TestPhysXBothStrategiesHybrid);
    RUN_TEST(walkercore::TestPhysXStrategyTagIsCanonical);
    RUN_TEST(walkercore::TestPhysXDumpVtableEntriesReportsFirstNVtableRun);
    RUN_TEST(walkercore::TestPhysXDumpVtableEntriesNoMatchReturnsZero);
    RUN_TEST(walkercore::TestPhysXDumpVtableEntriesRejectsNullBuffer);
    RUN_TEST(walkercore::TestPhysXDumpVtableEntriesAnnotatedLabelsCatalogMatches);
    RUN_TEST(walkercore::TestPhysXResolveKnownMethodsFindsReleaseAtSlotZero);
    RUN_TEST(walkercore::TestPhysXResolveKnownMethodsAllEightSlotsInOrder);
    RUN_TEST(walkercore::TestPhysXResolveKnownMethodsNoMatchReturnsFalse);
    RUN_TEST(walkercore::TestPhysXResolveKnownMethodsRespectsBounds);
    RUN_TEST(walkercore::TestPhysXKnownMethodsCatalogIsComplete);
    RUN_TEST(walkercore::TestPhysXKnownMethodsSweepIsTheHighestVA);
    RUN_TEST(walkercore::TestPhysXKnownMethodsCatalogVAsInPhysXRange);
    RUN_TEST(walkercore::TestPhysXKnownMethodsCatalogNamesMatchSDK);
    RUN_TEST(walkercore::TestPhysXKnownMethodsCatalogOrderIsStable);
    RUN_TEST(walkercore::TestPhysXSweepSecondaryResolvesCatalogSweepAtSlotFour);
    RUN_TEST(walkercore::TestPhysXSweepSecondaryReturnsNegativeOneWhenAbsent);
    RUN_TEST(walkercore::TestPhysXSweepSecondaryMatchesWithinTolerance);
    RUN_TEST(walkercore::TestPhysXSweepSecondaryRejectsOffsetBeyondTolerance);
    RUN_TEST(walkercore::TestPhysXSweepSecondaryHonoursOffset);
    RUN_TEST(walkercore::TestPhysXSweepSecondaryConstantsMatchCatalog);
#ifdef WALKERCORE_REAL_BINARY_TEST_PATH
    RUN_TEST(walkercore::TestPhysXRealBinaryMatchesCatalog);
    RUN_TEST(walkercore::TestPhysXRealBinaryRuntimeScanPath);
#endif

    // M5 PerTruckDoorTable probes — per model_id door geometry overrides.
    RUN_TEST(walkercore::TestPerTruckDoorTableBuiltInFindsKnownRow);
    RUN_TEST(walkercore::TestPerTruckDoorTableBuiltInUnknownReturnsNull);
    RUN_TEST(walkercore::TestPerTruckDoorTableBuiltInHasMultipleAtsRows);
    RUN_TEST(walkercore::TestPerTruckDoorTableNewRowsAreFindable);
    RUN_TEST(walkercore::TestApplyOverrideNullIsNoOp);
    RUN_TEST(walkercore::TestApplyOverrideDoorPatchIsSentinelAware);
    RUN_TEST(walkercore::TestApplyOverrideCabinBoxPatch);
    RUN_TEST(walkercore::TestTruckDoorConfigAppliesOverride);

    // M2 Game-Free-Camera shim probes — send-disabled by the test
    // build (WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED=1), so no
    // real keystrokes fire; the tests only inspect the shim's
    // counters and supported / enabled flags.
    RUN_TEST(walkercore::TestGameFreeCameraShimDefaultStateIsDisabled);
    RUN_TEST(walkercore::TestGameFreeCameraShimSetEnabledRoundTrip);
    RUN_TEST(walkercore::TestGameFreeCameraShimVkCustomisation);
    RUN_TEST(walkercore::TestGameFreeCameraShimOnEnterWalkingDisabledIsAdvisory);
    RUN_TEST(walkercore::TestGameFreeCameraShimOnEnterWalkingSupportedFalseIsAdvisory);
    RUN_TEST(walkercore::TestGameFreeCameraShimTriggerToggleRespectsDisabled);
    RUN_TEST(walkercore::TestGameFreeCameraShimEchoSuppressionAfterTrigger);
    // BindGameHwnd + PostMessage path — see test_game_free_camera_shim.cpp.
    RUN_TEST(walkercore::TestGameFreeCameraShimDefaultHwndIsNull);
    RUN_TEST(walkercore::TestGameFreeCameraShimBindGameHwndRoundTrip);
    RUN_TEST(walkercore::TestGameFreeCameraShimBindGameHwndOverwrites);
    RUN_TEST(walkercore::TestGameFreeCameraShimBindGameHwndNullClears);
    RUN_TEST(walkercore::TestGameFreeCameraShimDispatchSkipsWithoutHwnd);
    RUN_TEST(walkercore::TestGameFreeCameraShimDispatchUsesSendInputWhenHwndBound);
    RUN_TEST(walkercore::TestGameFreeCameraShimRebindOnEntryCatchesLateWindow);
    RUN_TEST(walkercore::TestGameFreeCameraShimOnExitWalkingLeavesCountersIntact);
    RUN_TEST(walkercore::TestGameFreeCameraShimOnExitWalkingDispatchesWhenSupported);
    RUN_TEST(walkercore::TestGameFreeCameraShimOnExitWalkingHonoursDisabledGate);
    RUN_TEST(walkercore::TestGameFreeCameraShimOnExitWalkingHonoursUnsupportedGate);
    RUN_TEST(walkercore::TestGameFreeCameraShimEntryAndExitToggleParity);
    RUN_TEST(walkercore::TestGameFreeCameraShimProbeConfigCandidatesCoversOneDrive);
    RUN_TEST(walkercore::TestGameFreeCameraShimProbeConfigCandidatesATSFirst);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadGDeveloperValueEmptyOnEmptyPath);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadGDeveloperValueParsesOne);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadGDeveloperValueParsesZero);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadGDeveloperValueEmptyOnMissingKey);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadGDeveloperValueMalformedNoQuotes);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadGDeveloperValueStopsAtFirstLine);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyEmptyOnEmptyPath);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyFindsKeyboardBinding);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyFindsAlternateName);
RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyFindsCamdbg);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyPicksKeyboardAmongMany);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyEmptyOnMissingAction);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyOnlyJoystickBinding);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyHandlesMalformedNoBacktick);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyDiagPlainFormNoTypo);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyDiagEscapedFormSetsTypo);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyDiagEscapedFormNoKeyboard);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyDiagMissingActionNoTypo);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyDiagNullOutParam);
    RUN_TEST(walkercore::TestGameFreeCameraShimReadControlsSiiKeyDiagEscapedWalkMode);
    RUN_TEST(walkercore::TestGameFreeCameraShimScsKeyNameToVkEmptyIsZero);
    RUN_TEST(walkercore::TestGameFreeCameraShimScsKeyNameToVkWrongPrefixIsZero);
    RUN_TEST(walkercore::TestGameFreeCameraShimScsKeyNameToVkFunctionKeys);
    RUN_TEST(walkercore::TestGameFreeCameraShimScsKeyNameToVkNumpad);
    RUN_TEST(walkercore::TestGameFreeCameraShimScsKeyNameToVkModifiersAndSpecial);
    RUN_TEST(walkercore::TestGameFreeCameraShimScsKeyNameToVkCursorAndLetters);
    RUN_TEST(walkercore::TestGameFreeCameraShimScsKeyNameToVkUnknownTokenIsZero);
    RUN_TEST(walkercore::TestGameFreeCameraShimProbeControlsSiiCandidatesCoversOneDrive);
    RUN_TEST(walkercore::TestGameFreeCameraShimAutoDetectAdoptsBindingWhenFound);
    RUN_TEST(walkercore::TestGameFreeCameraShimAutoDetectNoBindingLeavesDefaultVk);
    RUN_TEST(walkercore::TestGameFreeCameraShimAutoDetectInitialFieldsAreEmpty);
    RUN_TEST(walkercore::TestGameFreeCameraShimAutoEnableRequiresIniNotExplicit);
    RUN_TEST(walkercore::TestGameFreeCameraShimAutoEnableRequiresBinding);
    RUN_TEST(walkercore::TestGameFreeCameraShimAutoEnableRequiresSupported);
    RUN_TEST(walkercore::TestGameFreeCameraShimAutoEnableIsIdempotent);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherDefaultStateIsEnabled);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherSetEnabledRoundTrip);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherSetNameRvaRoundTrip);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherPatchOnceSkipsWhenNoModule);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherPatchOnceSkipsWhenDisabled);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherPatchOncePatchesFromZeroToOne);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherPatchOnceIsIdempotent);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherPatchOnceSkipsWhenValueAlreadyOne);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherPatchOnceDetectsSignatureMismatch);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherCountersAfterSuccessfulPatch);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherProbesValueAtStructOffset24);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherWiderScanWindowFindsFarStruct);
    RUN_TEST(walkercore::TestDevCameraCvarPatcherProbesValueAtStructOffset32);
    RUN_TEST(walkercore::TestCameraHijackComputeDispatcherVaZero);
    RUN_TEST(walkercore::TestCameraHijackComputeDispatcherVaAtsKbExe);
    RUN_TEST(walkercore::TestCameraHijackComputeDispatcherVaEts2);
    RUN_TEST(walkercore::TestCameraHijackIsRvaExecutableInText);
    RUN_TEST(walkercore::TestCameraHijackIsRvaExecutableAtBoundary);
    RUN_TEST(walkercore::TestCameraHijackIsRvaExecutableAtBoundaryJustAbove);
    RUN_TEST(walkercore::TestCameraHijackIsRvaExecutableInRdata);
    RUN_TEST(walkercore::TestCameraHijackIsRvaExecutableOutOfAllSections);
    RUN_TEST(walkercore::TestCameraHijackIsRvaExecutableEmptyTable);
    RUN_TEST(walkercore::TestCameraHijackDispatcherRvaInText);
    RUN_TEST(walkercore::TestCameraHijackComputeSingletonGlobalVaZero);
    RUN_TEST(walkercore::TestCameraHijackComputeSingletonGlobalVaAtsKbExe);
    RUN_TEST(walkercore::TestCameraHijackComputeVtableSlot16VaZero);
    RUN_TEST(walkercore::TestCameraHijackComputeVtableSlot16VaAtsKbExe);
    RUN_TEST(walkercore::TestPxSceneHookCatalogConstantsMatchAmtrucksLayout);
    RUN_TEST(walkercore::TestPxSceneHookFreshInstanceIsZero);
    RUN_TEST(walkercore::TestPxSceneHookObserveStoresFirstCapture);
    RUN_TEST(walkercore::TestPxSceneHookObserveKeepsFirstCaptureOnLaterNonZeroCalls);
    RUN_TEST(walkercore::TestPxSceneHookObserveIgnoresZero);
    RUN_TEST(walkercore::TestPxSceneHookResetForTestingClearsState);
    RUN_TEST(walkercore::TestPxSceneHookInstallWithoutAmtrucksLeavesTargetUnreachable);
    RUN_TEST(walkercore::TestPxSceneHookInstallUninstallRoundtripIsIdempotent);
    RUN_TEST(walkercore::TestImGuiOverlayDefaultStateIsDisabled);
    RUN_TEST(walkercore::TestImGuiOverlaySetEnabledRoundTrip);
    RUN_TEST(walkercore::TestImGuiOverlayOnEnterWalkingIncrementsCounter);
    RUN_TEST(walkercore::TestImGuiOverlayOnExitWalkingIncrementsCounter);
    RUN_TEST(walkercore::TestImGuiOverlayCountersStayIndependent);
    RUN_TEST(walkercore::TestImGuiOverlayResetForTestingIsSafe);
    RUN_TEST(walkercore::TestImGuiOverlayVersionStringIsNonNull);
    RUN_TEST(walkercore::TestImGuiOverlayInitializeRefusesNullDevice);
    RUN_TEST(walkercore::TestImGuiOverlayInitializeIsIdempotent);
    RUN_TEST(walkercore::TestImGuiOverlayRenderNoOpWhenDisabled);
    RUN_TEST(walkercore::TestImGuiOverlayRenderNoOpWhenSupportedFalse);
    RUN_TEST(walkercore::TestImGuiOverlayOnDeviceLostIsSafeWhenUninitialized);

    // M11.5 — button click counter (interactive widgets).
    RUN_TEST(walkercore::TestImGuiOverlayButtonClicksStartsAtZero);
    RUN_TEST(walkercore::TestImGuiOverlayButtonClicksResetsBetweenOverlays);
    RUN_TEST(walkercore::TestImGuiOverlayBindFrameLoopRoundTrip);
    RUN_TEST(walkercore::TestImGuiOverlayBindFsmRoundTrip);
    RUN_TEST(walkercore::TestImGuiOverlayResetForTestingClearsBindings);
    RUN_TEST(walkercore::TestImGuiOverlayBindSettingsMenuRoundTrip);
    RUN_TEST(walkercore::TestImGuiOverlayBindSettingsMenuIndependentFromOtherBindings);

    // M9 SCS configuration event + CouplingProvider real-hook probes.
    RUN_TEST(walkercore::TestScsTelemetryProviderRegisterEventsNullIsSafe);
    RUN_TEST(walkercore::TestScsTelemetryProviderUnregisterEventsNullIsSafe);
    RUN_TEST(walkercore::TestCouplingProviderPlaceholderWaypointWhenHookUnset);
    RUN_TEST(walkercore::TestCouplingProviderRealHookPositionTransformsToWorld);
    RUN_TEST(walkercore::TestCouplingProviderRealHookPositionRotatesWithTruckHeading);
    RUN_TEST(walkercore::TestCouplingProviderRealHookLandingGearStep);
    RUN_TEST(walkercore::TestCouplingProviderHookUnsetUsesTruckLocalFallback);
    // M9 Lander: telemetry-driven ForceComplete via
    // OnTrailerAttachStateChanged.
    RUN_TEST(walkercore::TestCouplingOnTrailerAttachIdleIsNoOp);
    RUN_TEST(walkercore::TestCouplingOnTrailerAttachCompleteIsNoOp);
    RUN_TEST(walkercore::TestCouplingOnTrailerAttachTrueMidFsmForcesComplete);
    RUN_TEST(walkercore::TestCouplingOnTrailerAttachFalseMidFsmResetsToIdle);
    RUN_TEST(walkercore::TestCouplingOnTrailerAttachIdempotentAcrossIdenticalTransitions);

    // M7 opentrack UDP receiver probes.
    RUN_TEST(walkercore::TestOpentrackReceiverInstallUninstallIdempotent);
    RUN_TEST(walkercore::TestOpentrackReceiverTickWithoutInstallIsSafe);
    RUN_TEST(walkercore::TestOpentrackReceiverTickWithNullProviderDrainsSocket);
    RUN_TEST(walkercore::TestOpentrackReceiverRoundTripFeedsDeltasIntoProvider);
    RUN_TEST(walkercore::TestOpentrackReceiverShortestArcDeltaOnYawWrap);

    // M5 TruckDoorConfigFromTruckState probes.
    RUN_TEST(walkercore::TestTruckDoorConfigFromStateFallsBackWhenAnchorMissing);
    RUN_TEST(walkercore::TestTruckDoorConfigFromStateAnchorsDoorToCabin);
    RUN_TEST(walkercore::TestTruckDoorConfigFromStateDriverSeatFromHeadPosition);
    RUN_TEST(walkercore::TestTruckDoorConfigFromStateOnlyCabinSetFallsBack);

    RUN_TEST(walkercore::TestLogCustomSinkReceivesAllLevels);
    RUN_TEST(walkercore::TestLogLevelReachesSink);
    RUN_TEST(walkercore::TestLogShutdownIdempotent);
    RUN_TEST(walkercore::TestLogNullSinkIsSafe);

    // M8 fuel telemetry sync.
    RUN_TEST(walkercore::TestFuelProviderSyncFromTelemetry);
    RUN_TEST(walkercore::TestFuelProviderRetainsLocalWhenTelemetryMissing);
    RUN_TEST(walkercore::TestFuelProviderFuelingAdvancesAfterTelemetry);
    RUN_TEST(walkercore::TestFuelStationReadAfterTickReflectsCurrentPlayerPosition);
    // M8 Lander: per-truck tank capacities from the SCS
    // configuration event.
    RUN_TEST(walkercore::TestFuelProviderCapacityFromConfigEventOverridesStub);
    RUN_TEST(walkercore::TestFuelProviderFuelingClampsToRealPerTruckCapacity);
    RUN_TEST(walkercore::TestFuelProviderStubCapacityHoldsWithoutConfigEvent);
    RUN_TEST(walkercore::TestFuelProviderAdblueCapacityFromConfigEventOverridesStub);

    // Diagnostic dump (walker_dump_state).
    RUN_TEST(walkercore::TestDiagnosticDumpSafeBeforeInit);
    RUN_TEST(walkercore::TestDiagnosticDumpReportsScsVersion);
    RUN_TEST(walkercore::TestDiagnosticDumpMirrorsFsmLabel);
    // F12 / per-frame FSM label cache probes. The per-frame conductor
    // (walker_per_frame_tick) consults FsmLabelNeedsReencode before
    // running EncodeFsmLabel + SetFsmLabel; in steady state (Driving
    // or Walking with no change) the work is skipped. These tests pin
    // the encoder + cache-decision behaviour without needing the full
    // D3D11 / InputHook stack.
    RUN_TEST(walkercore::TestEncodeFsmLabelEveryState);
    RUN_TEST(walkercore::TestEncodeFsmLabelProgressOnlyForTransitions);
    RUN_TEST(walkercore::TestEncodeFsmLabelClampsProgressToUnitInterval);
    RUN_TEST(walkercore::TestEncodeFsmLabelBoundedOutput);
    RUN_TEST(walkercore::TestDumpKnockoutHandlesUninitialized);
    RUN_TEST(walkercore::TestDumpKnockoutReportsStateAndConfig);
    RUN_TEST(walkercore::TestDumpPerTruckDoorHandlesEmpty);
    RUN_TEST(walkercore::TestDumpPerTruckDoorReportsMatchedRow);
    RUN_TEST(walkercore::TestDumpPerTruckDoorReportsUnmatchedRow);
    RUN_TEST(walkercore::TestDumpCouplingHandlesUninitialized);
    RUN_TEST(walkercore::TestDumpCouplingReportsActiveStep);
    RUN_TEST(walkercore::TestDumpBuildInfoEmitsBannerAndFields);
    RUN_TEST(walkercore::TestFsmLabelNeedsReencode);
    RUN_TEST(walkercore::TestFsmLabelNeedsReencodeSteadyStateIsFalse);
    RUN_TEST(walkercore::TestFsmLabelNeedsReencodeTransitionAlwaysTrue);
    RUN_TEST(walkercore::TestFsmLabelCanonicalBuffer);
    RUN_TEST(walkercore::TestFsmLabelCacheDecisionDrivesEncodeCalls);
    RUN_TEST(walkercore::TestDiagnosticDumpReportsFuelAndAdBlue);
    RUN_TEST(walkercore::TestDiagnosticDumpReportsFrameLoopCamera);
    RUN_TEST(walkercore::TestDiagnosticDumpReportsInputState);
    RUN_TEST(walkercore::TestDiagnosticDumpReportsFuelProvider);
    RUN_TEST(walkercore::TestDiagnosticDumpReportsKnockout);
    RUN_TEST(walkercore::TestDiagnosticDumpReportsD3D11Hook);
    RUN_TEST(walkercore::TestDiagnosticDumpSettingsReportsBeforeInit);
    RUN_TEST(walkercore::TestDiagnosticDumpSettingsReportsLiveValues);
    RUN_TEST(walkercore::TestDiagnosticDumpReportsPlayerModeName);
    RUN_TEST(walkercore::TestPlayerModeNameReturnsNamesForAllValues);

    // walker_dump_menu() focused dump + walker_dump_state() row.
    RUN_TEST(walkercore::TestDiagnosticDumpMenuReportsNotInitialized);
    RUN_TEST(walkercore::TestDiagnosticDumpMenuReportsClosedClean);
    RUN_TEST(walkercore::TestDiagnosticDumpMenuReportsOpenClean);
    RUN_TEST(walkercore::TestDiagnosticDumpMenuReportsOpenDirty);
    RUN_TEST(walkercore::TestDiagnosticDumpMenuMirrorsClose);
    RUN_TEST(walkercore::TestDiagnosticDumpFullStateMenuSection);

    // walker_dump_head_tracker() focused dump.
    RUN_TEST(walkercore::TestDiagnosticDumpHeadTrackerReportsNotInitialized);
    RUN_TEST(walkercore::TestDiagnosticDumpHeadTrackerReportsConfigAndState);
    RUN_TEST(walkercore::TestDiagnosticDumpHeadTrackerReportsActiveAfterMouseFeed);
    RUN_TEST(walkercore::TestDiagnosticDumpHeadTrackerPeekDoesNotPerturb);

    // walker_dump_shadow() focused dump.
    RUN_TEST(walkercore::TestDiagnosticDumpShadowReportsNotInitialized);
    RUN_TEST(walkercore::TestDiagnosticDumpShadowReportsNoPlayerState);
    RUN_TEST(walkercore::TestDiagnosticDumpShadowReportsConfigAndProjection);
    RUN_TEST(walkercore::TestDiagnosticDumpShadowSunOnHorizonEdge);

    // walker_dump_flashlight() + walker_dump_door() focused dumps.
    RUN_TEST(walkercore::TestDiagnosticDumpFlashlightReportsNotInitialized);
    RUN_TEST(walkercore::TestDiagnosticDumpFlashlightReportsNoCameraState);
    RUN_TEST(walkercore::TestDiagnosticDumpFlashlightReportsOffByDefault);
    RUN_TEST(walkercore::TestDiagnosticDumpFlashlightReportsConeAfterToggle);
    RUN_TEST(walkercore::TestDiagnosticDumpDoorReportsNotInitialized);
    RUN_TEST(walkercore::TestDiagnosticDumpDoorReportsClosedIdle);
    RUN_TEST(walkercore::TestDiagnosticDumpDoorReportsMidwayAnimation);

    // walker_dump_version() focused dump.
    RUN_TEST(walkercore::TestDiagnosticDumpVersionReportsNotInitialized);
    RUN_TEST(walkercore::TestDiagnosticDumpVersionReportsEts2Match);
    RUN_TEST(walkercore::TestDiagnosticDumpVersionReportsAtsMatch);
    RUN_TEST(walkercore::TestDiagnosticDumpVersionReportsNoMatchForUnknownBuild);

    // walker_dump_fsm() focused dump.
    RUN_TEST(walkercore::TestDiagnosticDumpFsmReportsNotInitialized);
    RUN_TEST(walkercore::TestDiagnosticDumpFsmReportsDrivingDefault);
    RUN_TEST(walkercore::TestDiagnosticDumpFsmReportsMidTransitionOut);
    RUN_TEST(walkercore::TestDiagnosticDumpFsmReportsMidTransitionIn);

    // walker_dump_input() focused dump.
    RUN_TEST(walkercore::TestDiagnosticDumpInputReportsNotInitialized);
    RUN_TEST(walkercore::TestDiagnosticDumpInputReportsEmptyState);
    RUN_TEST(walkercore::TestDiagnosticDumpInputReportsPerKeyEdges);
    RUN_TEST(walkercore::TestDiagnosticDumpInputQuietKeysAreOmitted);

    // SettingsMenu (M11 stub) edit cycle.
    RUN_TEST(walkercore::TestSettingsMenuOpenClosesDirty);
    RUN_TEST(walkercore::TestSettingsMenuSetSliderKnownKeyWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuSetSliderJumpVelocityWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuSetSliderGravityWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuSetSliderMaxStepUpWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuSetSliderMaxSlopeAngleWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuSetSliderNearZWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuSetSliderFarZWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuSetSliderUnknownKeyIsSilent);
    RUN_TEST(walkercore::TestSettingsMenuEveryDocumentedSliderWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuEveryDocumentedBoolWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuEveryDocumentedKeyBindingWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuUnknownKeysAreSilentAcrossAllApis);
    RUN_TEST(walkercore::TestSettingsMenuSetBoolUseMouseDirties);
    RUN_TEST(walkercore::TestSettingsMenuSetBoolUnknownKeyIsSilent);
    RUN_TEST(walkercore::TestSettingsMenuSetBoolUseTrackirDirties);
    RUN_TEST(walkercore::TestSettingsMenuSetBoolUseOpentrackDirties);
    RUN_TEST(walkercore::TestSettingsMenuSetBoolApplyAllPushesToProvider);
    RUN_TEST(walkercore::TestSettingsMenuSetKeyBindingKnownKeyWritesThrough);
    RUN_TEST(walkercore::TestSettingsMenuSetKeyBindingUnknownKeyIsSilent);
    RUN_TEST(walkercore::TestSettingsMenuToggleFlipsIsOpen);
    RUN_TEST(walkercore::TestSettingsMenuCloseWithoutDirtyDoesNotWrite);
    RUN_TEST(walkercore::TestSettingsMenuCloseWithDirtyWritesToDisk);

    // SettingsStore::CheckFileChanged() — per-frame polling contract.
    // Behavioural probes pin the missing-file / empty-path / no-change /
    // mtime-bump / rapid-edits / SetCurrent-clobber contracts.
    RUN_TEST(walkercore::TestSettingsCheckFileChangedEmptyPathReturnsFalse);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedMissingFileReturnsFalse);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedNoChangeReturnsFalse);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedReloadsOnMtimeChange);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedRapidEditsDetected);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedReloadOverwritesSetCurrent);
    // Throttle contract (CheckFileChanged self-limits to one
    // GetFileAttributesEx per kCheckPollingIntervalUs ≈ 250 ms).
    RUN_TEST(walkercore::TestSettingsCheckFileChangedThrottleFirstCallPolls);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedThrottleSkipsRapidCalls);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedThrottleResetHookUnblocksNextPoll);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedThrottleResetIsTransparentToMtimeCheck);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedThrottlePollingIntervalIsExposed);
    // Perf budgets — both the throttle-skip path (sub-µs) and the
    // actual-poll path (50 µs, one syscall).
    RUN_TEST(walkercore::TestSettingsCheckFileChangedThrottleSkipPathPerfBudget);
    RUN_TEST(walkercore::TestSettingsCheckFileChangedPollPathPerfBudget);
    RUN_TEST(walkercore::TestSettingsStoreApplyAllPerfBudget);

    // WalkerFsm::Update steady-state perf budget — the most common
    // per-frame FSM call (state == Driving, no key press). Pin at
    // 20 µs/call so a regression here doesn't quietly compound with
    // the FrameLoop + Sample budget at 200 µs/tick.
    RUN_TEST(walkercore::TestWalkerFsmUpdateSteadyStatePerfBudget);
    RUN_TEST(walkercore::TestWalkerFsmUpdateMidTransitionPerfBudget);

    // ScsTelemetryProvider direct probes — pins the default-state
    // invariants (every has_* is false; scalars are zero), the null-
    // pointer safety contract (RegisterAll/UnregisterAll with nullptr),
    // and the channel-list stability (every registered channel name is
    // a non-empty string in the vendored SDK headers).
    RUN_TEST(walkercore::TestScsTelemetryProviderFreshStateAllHasFlagsAreFalse);
    RUN_TEST(walkercore::TestScsTelemetryProviderFreshStateScalarFieldsAreZero);
    RUN_TEST(walkercore::TestScsTelemetryProviderStateIsStableAcrossConstAccess);
    RUN_TEST(walkercore::TestScsTelemetryProviderRegisterAllNullIsSafe);
    RUN_TEST(walkercore::TestScsTelemetryProviderUnregisterAllNullIsSafe);

    // D3D11Hook state-machine probes (M2 lander) — pin the
    // Install/Uninstall invariants, the null-safety contract, and the
    // generation-counter reset point. The actual per-frame Present
    // hook is verified by integration, not here.
    RUN_TEST(walkercore::TestD3D11HookFreshInstanceIsNotInstalled);
    RUN_TEST(walkercore::TestD3D11HookFreshSwapChainGenerationIsZero);
    RUN_TEST(walkercore::TestD3D11HookBindFrameLoopNullIsSafe);
    RUN_TEST(walkercore::TestD3D11HookBindFrameLoopStoresPointer);
    RUN_TEST(walkercore::TestD3D11HookUninstallWithoutInstallIsSafe);
    RUN_TEST(walkercore::TestD3D11HookInstallUninstallRoundtrip);
    RUN_TEST(walkercore::TestD3D11HookSecondInstallReturnsFalse);
    RUN_TEST(walkercore::TestD3D11HookDestructorOnFreshInstanceIsSafe);
    RUN_TEST(walkercore::TestD3D11HookDestructorAfterUninstallIsSafe);

    // D3D12Hook state-machine probes (M2 lander D3D12 path) — pin
    // the same Install/Uninstall invariants as the D3D11 path, and
    // pin that the two swap-chain generation counters are
    // independent globals. The D3D12 chain (D3D12CreateDevice →
    // CreateDXGIFactory1 → IDXGIFactory::CreateSwapChain vtable →
    // IDXGISwapChain::Present vtable) is verified by integration
    // against a real D3D12 process, not here.
    RUN_TEST(walkercore::TestD3D12HookFreshInstanceIsNotInstalled);
    RUN_TEST(walkercore::TestD3D12HookFreshSwapChainGenerationIsZero);
    RUN_TEST(walkercore::TestD3D12HookBindFrameLoopNullIsSafe);
    RUN_TEST(walkercore::TestD3D12HookBindFrameLoopStoresPointer);
    RUN_TEST(walkercore::TestD3D12HookUninstallWithoutInstallIsSafe);
    RUN_TEST(walkercore::TestD3D12HookInstallUninstallRoundtrip);
    RUN_TEST(walkercore::TestD3D12HookSecondInstallReturnsFalse);
    RUN_TEST(walkercore::TestD3D12HookDestructorOnFreshInstanceIsSafe);
    RUN_TEST(walkercore::TestD3D12HookDestructorAfterUninstallIsSafe);
    RUN_TEST(walkercore::TestD3D11AndD3D12GenerationCountersAreIndependent);
    RUN_TEST(walkercore::TestD3D12HookInstallStateSurvivesSecondUninstall);
    RUN_TEST(walkercore::TestD3D11AndD3D12ShareProcessWideMinHook);

    // PerFrameThread background ticker — fallback lander for the
    // case where the D3D Present hooks never fire (SCS plugins
    // load AFTER the game's swap chain is already up).
    RUN_TEST(walkercore::TestPerFrameThreadStartStopIsIdempotent);
    RUN_TEST(walkercore::TestPerFrameThreadTicksAdvanceOverWindow);
    RUN_TEST(walkercore::TestPerFrameTickLockSerializesTwoCallers);
    RUN_TEST(walkercore::TestPerFrameThreadSurvivesManyStartStopCycles);

    // Time.h contract probes — the per-frame conductor and the
    // settings store both rely on NowMicros() being a real
    // monotonic clock and DeltaSeconds() being well-behaved at
    // all edge cases (0, negative, microsecond precision).
    RUN_TEST(walkercore::TestTimeNowMicrosReturnsNonZero);
    RUN_TEST(walkercore::TestTimeNowMicrosIsMonotonic);
    RUN_TEST(walkercore::TestTimeNowMicrosAdvancesOverSleep);
    RUN_TEST(walkercore::TestTimeDeltaSecondsPositive);
    RUN_TEST(walkercore::TestTimeDeltaSecondsZeroWhenEqual);
    RUN_TEST(walkercore::TestTimeDeltaSecondsClampedWhenNegative);
    RUN_TEST(walkercore::TestTimeDeltaSecondsHandlesRealisticDelta);
    RUN_TEST(walkercore::TestTimeDeltaSecondsMicrosecondPrecision);

    TEST_SUMMARY();
}