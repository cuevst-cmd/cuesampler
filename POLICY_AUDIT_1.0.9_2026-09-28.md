# CUE SAMPLER 1.0.9 — policy and release audit

Reviewed September 28, 2026. Recommendation: resolve the confirmed discrepancies below before packaging 1.0.9. This is a technical consistency review with legal issues identified for counsel, not an enforceability opinion.

Scope: repository EULA, installer license, privacy policy, third-party notices, relevant implementation and packaging scripts; live cuesampler.com navigation and embedded policy pages; both Mac and Windows product listings; existing local 1.0.8 macOS package payload. No new installer was built and no live policies were changed. Existing application edits were preserved.

## 1. Correct privacy promises before release

**Confirmed: published policies and repository policies disagree.** The live [policy pages](https://www.cuesampler.com/landing/embed) still describe optional telemetry, an in-plugin consent toggle, installation identifiers, and a Google endpoint. The current repository says that feature was removed. Publish a single, consistent revision across website, installer, and repository. Keep dated older versions available for reference.

**Confirmed: the repository overstates what the software does not store.** [EULA.md](/Volumes/PMCO/REPOS/SAMPLERv3/EULA.md:54) and [PRIVACY_POLICY.md](/Volumes/PMCO/REPOS/SAMPLERv3/PRIVACY_POLICY.md:19) deny collection, storage, and transmission of audio, filenames, fingerprints, and analysis results. Local processing and storage are normal product functions:

- [PluginProcessor.cpp](/Volumes/PMCO/REPOS/SAMPLERv3/PluginProcessor.cpp:3899) puts the source path and embedded audio into saved plugin state.
- [StemCache.cpp](/Volumes/PMCO/REPOS/SAMPLERv3/StemCache.cpp:185) uses a local stem-cache directory; line 218 hashes audio for cache keys.
- [UpdateChecker.cpp](/Volumes/PMCO/REPOS/SAMPLERv3/UpdateChecker.cpp:333) maintains a local update-information cache.

Describe local processing/storage separately from information sent to CUE or providers. Do not remove useful project persistence merely to make the current wording true.

**Confirmed implementation; dependency behavior requires verification:** CUE's own former telemetry code is absent from the reviewed active application sources, but both [BeatThisAnalyzer.cpp](/Volumes/PMCO/REPOS/SAMPLERv3/BeatThisAnalyzer.cpp:29) and [StemSeparator.cpp](/Volumes/PMCO/REPOS/SAMPLERv3/StemSeparator.cpp:199) initialize ONNX Runtime without explicitly disabling its telemetry. Windows uses the official 1.20.1 DirectML package. Microsoft's [version-specific privacy notice](https://raw.githubusercontent.com/microsoft/onnxruntime/v1.20.1/docs/Privacy.md) documents default-enabled Windows diagnostic tracing and possible OS-mediated transmission subject to user consent. This is not proof that customer data was sent. Explicitly disable supported runtime telemetry, verify the actual Windows artifact, and assess the shared runtime/host behavior before promising the entire product has no telemetry. Do not extrapolate newer ONNX versions' behavior to the pinned macOS dependency.

**Update checks should be explicit.** [PluginProcessor.cpp](/Volumes/PMCO/REPOS/SAMPLERv3/PluginProcessor.cpp:2327) starts the updater automatically. It also supports manual checks; automatic checks use a one-day throttle. Requests reach GitHub's releases API with a generic User-Agent and ordinary connection metadata. State this plainly, including local caching, rather than relying only on “may” make a request. No installation identifier appears in the reviewed request code. This was a source review, not a network capture of every dependency.

**Historical telemetry remains an open business fact.** Local cleanup of old telemetry files does not delete previously uploaded records. Confirm whether Google-hosted records, spreadsheets, exports, or backups remain; document actual retention, deletion handling, and provider access. Do not call retained persistent identifiers or audio hashes inherently anonymous.

## 2. Repair the privacy-policy link

**Confirmed:** the EULA's privacy URL leads to [a 404 page](https://www.cuesampler.com/privacy). The site's internal Privacy navigation works inside its embedded landing page, but that does not repair the URL distributed in the installer.

Provide stable public privacy and EULA URLs or redirects, update all references, and verify direct navigation without a login. Put accessible policy links on individual product pages before purchase as well as in the homepage footer.

## 3. Include the promised third-party materials in macOS distributions

**Confirmed packaging gap:** [THIRD_PARTY_NOTICES.txt](/Volumes/PMCO/REPOS/SAMPLERv3/THIRD_PARTY_NOTICES.txt:4) says license texts accompany it and identifies a modified Bungee source archive. The [Windows packaging script](/Volumes/PMCO/REPOS/SAMPLERv3/make-installer-windows.ps1:182) stages these materials. The Mac path does not have equivalent staging.

Read-only inspection found:

- The current built AU and VST3 resource directories contain models and VST3 metadata, with no license directory or source archive.
- `pkgutil --payload-files dist/CUESAMPLER-1.0.8.pkg` listed 73 entries and no license, notice, or ZIP basenames. The installer license screen is distinct from an installed notices bundle.

For 1.0.9, stage applicable license/copyright notices and the matching modified Bungee source before signing the bundles. Cover AU, VST3, and standalone whenever distributed. Include required notices for transitive dependencies, models, and the Syne font; validate that the source archive contains the actual patched source and versioned submodules used by the build.

[MPL 2.0, sections 3.1–3.3](https://www.mozilla.org/en-US/MPL/2.0/) requires covered source availability and recipient notice when distributing executable form. A reliable source-download mechanism can also meet the obligation; an included archive is this project's existing approach. A generic upstream link does not identify CUE's local modifications. This does not require publishing the entire proprietary application; see [Mozilla's larger-work guidance](https://www.mozilla.org/en-US/MPL/2.0/FAQ/).

Correct the platform inventory: macOS pins ONNX Runtime **1.18.1**, Windows **1.20.1**, and DirectML is Windows-only. The shared notices currently describe only the Windows ONNX version. Preserve evidence tying each shipped model checkpoint/conversion to its upstream license; upstream [Beat This!](https://raw.githubusercontent.com/CPJKU/beat_this/main/LICENSE) and [Demucs](https://raw.githubusercontent.com/facebookresearch/demucs/main/LICENSE) license files are MIT, but that alone is not a complete provenance audit of every binary model.

Add an explicit third-party-license exception to EULA restrictions. The notices already contain one, but recipients should not have to reconcile blanket copying/modification restrictions without that explanation.

## 4. Align refund terms with consumer rights and Gumroad

[EULA section 5](/Volumes/PMCO/REPOS/SAMPLERv3/EULA.md:44) does already preserve mandatory statutory rights. Keep that protection. Its preceding absolute denial of refunds, including for defects, nevertheless gives customers a conflicting message. The [Mac](https://www.cuesampler.com/l/wgcah) and [Windows](https://www.cuesampler.com/l/yyzvfa) listings also display a no-refund policy.

Describe the policy as no *discretionary change-of-mind* refunds, subject to mandatory remedies and the platform's applicable policies. Gumroad allows seller policies but reserves refund discretion and specifically honors qualifying Brazilian withdrawal requests even for no-refund products. Therefore CUE cannot promise that refunds never occur. [Gumroad refund policy](https://gumroad.com/help/article/51-what-is-gumroads-refund-policy).

For applicable EU consumer sales, defective digital content can trigger correction and, in qualifying circumstances, price reduction or termination/refund. A beta label does not by itself remove mandatory rights. [European Commission guidance](https://commission.europa.eu/topics/business-and-industry/contract-rules/digital-contracts/digital-contract-rules_en).

Do not assume the EULA sentence about purchasing/downloading establishes every required immediate-delivery withdrawal waiver. Verify the actual regional checkout consent and acknowledgment process; withdrawal exceptions and defect remedies are distinct. [EU consumer guidance](https://europa.eu/youreurope/citizens/consumers/shopping/shopping-consumer-rights/index_en.htm). This review did not complete purchases or verify regional checkout flows.

Clarify roles: CUE licenses the software; Gumroad describes itself as merchant of record for its sales. Reflect the actual customer-data flow rather than describing all providers solely as payment processors. [Gumroad merchant-of-record guidance](https://gumroad.com/help/article/121-sales-tax-on-gumroad).

## 5. Correct public release and compatibility copy

The [live landing page](https://www.cuesampler.com/landing/embed) advertises beta 8, conflicting macOS 13+/11+ requirements, Pro Tools compatibility, and real-time stem separation.

Repository evidence and required actions:

- [CMakeLists.txt](/Volumes/PMCO/REPOS/SAMPLERv3/CMakeLists.txt:25) targets macOS 11.0. Choose a **tested support minimum**, then use it consistently; a build target alone does not prove compatibility.
- [Configured formats](/Volumes/PMCO/REPOS/SAMPLERv3/CMakeLists.txt:168) are AU, VST3, and standalone, with no AAX target. Pro Tools uses AAX plugins; remove unqualified native compatibility claims or describe a specifically tested third-party hosting arrangement. [Avid AAX documentation](https://developer.avid.com/aax/).
- [Stem processing](/Volumes/PMCO/REPOS/SAMPLERv3/PluginProcessor.cpp:5859) runs as a background job and caches its results. Use “on-device AI stem separation; processing time varies by sample and hardware” unless measured real-time guarantees are available.
- State which formats each purchased download actually installs. The inspected Mac package installs AU and VST3; standalone being built does not establish that it is included in that package.
- Align release number, beta/stable status, supported architectures, tested DAWs, minimum OS, download names, and release notes before purchase. Clarify automatic BPM/key estimates can need adjustment.

The [Windows listing](https://www.cuesampler.com/l/yyzvfa) also begins with generic approval instructions. Replace them with precise installation guidance and verified publisher information. The landing page also sells CUE-RACK; define its policy coverage separately where its licensing or price differs.

## 6. Complete privacy disclosures using real business facts

The repository already addresses contact/purchase information, website data, broad sharing purposes, user rights, security, children, and contact details. Useful foundations remain.

Before finalizing, confirm and document:

- Controller identity consistent with the EULA's Jerry Volpe, doing business as CUE Software, and an appropriate business contact/address where required.
- Actual provider roles and names, including checkout, email/newsletter, hosting, analytics, GitHub, and any remaining historical telemetry service.
- Retention periods or meaningful criteria for orders, support, newsletter subscriptions, and historical telemetry; distinguish records held by CUE from provider-controlled records.
- Applicable legal bases, international transfers/safeguards, withdrawal of consent, and regulator complaint rights where the relevant privacy law applies.
- Actual cookies/analytics and required consent behavior. Browser cookie settings are not a substitute for consent where prior consent is required.

The [ICO's disclosure checklist](https://ico.org.uk/for-organisations/uk-gdpr-guidance-and-resources/individual-rights/the-right-to-be-informed/what-privacy-information-should-we-provide/) is a useful UK GDPR reference. Applicability depends on actual markets and processing; this audit does not assume every privacy statute applies. No backend retention, email-provider account, or comprehensive cookie/network audit was performed. Do not invent durations or providers to fill the policy.

## 7. EULA clarity, licensing, and release mechanics

- **Beta status:** section 3 treats all software as beta. Confirm whether 1.0.9 remains beta before rewriting it. A version number alone does not answer this.
- **License consistency:** section 1 permits installations on owned/controlled computers, while section 2 permits only one backup copy. Clarify authorized installation copies and reasonable backups; decide personal versus multi-user studio licensing explicitly.
- **Audio ownership:** explain that CUE claims no ownership of users' input/output audio and that plugin use does not grant rights in third-party recordings. Stem separation is not sample clearance.
- **Counsel review:** mandatory consumer protections may affect the no-support/update obligation, revocable license, automatic termination, warranty/liability exclusions, and exclusive New York forum. Existing statutory savings language helps but is not a worldwide enforceability guarantee.
- **JUCE eligibility:** retain evidence of the applicable JUCE 8 plan and developer seats. Free Starter can permit commercial distribution when its eligibility requirements are met; a paid plan is not automatically required. The [current JUCE agreement](https://juce.com/legal/juce-8-licence/) defines revenue/funding and distribution conditions. The Windows script already asks for eligibility confirmation; verify it for Mac releases too.
- **Encoding and dates:** [LICENSE.txt](/Volumes/PMCO/REPOS/SAMPLERv3/LICENSE.txt:1) has a corrupted em dash in its title. Generate it consistently from the canonical EULA in UTF-8 and inspect the rendered installer screen. Publish accurate revision dates; the EULA date predates current privacy wording.
- **Version:** [CMakeLists.txt](/Volumes/PMCO/REPOS/SAMPLERv3/CMakeLists.txt:9) still says 1.0.8. Update the project version before release and reject installer overrides that disagree with binary metadata.
- **Notarization gate:** [make-installer.sh](/Volumes/PMCO/REPOS/SAMPLERv3/make-installer.sh:97) warns and continues after failed notarization submission, and suppresses final Gatekeeper failure. The release wrapper then reports readiness. Require successful notarization/stapling/verification before declaring a package shippable.

## Suggested wording for review

These are proposed clauses, not published replacements. Confirm dependency telemetry handling and the business facts above before adopting them.

**Local audio and project data:** “CUE SAMPLER processes your audio on your device. To restore your work and support playback and stem separation, it may store audio, source-file paths, analysis results, edit settings, and audio-derived cache keys in project files and local caches. CUE does not receive this content through the former optional data-sharing feature, which has been removed. Files you choose to send to support are handled as support information. Your DAW, operating system, or backup service may separately manage copies of your files.”

**Update checks:** “The Software automatically checks GitHub Releases for available updates, subject to a daily interval, and also supports manual checks. GitHub receives ordinary connection information, including your IP address and a generic application User-Agent. CUE's update request does not include your audio, filenames, analysis results, or an installation identifier. Update results and preferences are cached locally.”

**Third-party components:** “Third-party components are governed by their applicable licenses, identified in the accompanying notices. Nothing in this Agreement limits rights those licenses grant you in those components. Those terms control to the extent of a conflict concerning those components.”

**Refund policy:** “Except as required by applicable law or the applicable checkout platform's policies, we do not offer discretionary refunds for a change of mind after digital delivery. Nothing in this policy excludes mandatory consumer rights, including remedies for defective or misdescribed digital content. Contact cue@cuesampler.com for assistance with an order or defect.” Have counsel confirm the final wording and corresponding checkout treatment.

## Acceptance checks before creating the release package

1. Resolve beta/stable status, JUCE eligibility, historical telemetry retention, provider inventory, and tested compatibility minimums.
2. Review the revised canonical policies; reconcile website/product copy and working public legal URLs.
3. Verify dependency privacy behavior, particularly Windows ONNX, against the final shipped artifacts.
4. Stage platform-correct notices and matching modified Bungee source for every distribution; inspect archive contents and license precedence.
5. Set 1.0.9 consistently, generate a clean installer license screen, and make signing/notarization failures stop the release.
6. After packaging is authorized, inspect the final payload, legal links/text, signatures, and version metadata. Retain the approved policies and source/license inventory with that exact release.

The audit itself is complete. Unanswered business facts and unperformed runtime/checkout checks above remain explicit release follow-ups, not assumed approvals or verified facts.


## Resolution update — September 29, 2026

Implementation and live-site changes are recorded in [1.0.9 release readiness](/Volumes/PMCO/REPOS/SAMPLERv3/docs/releases/1.0.9-readiness.md). The findings above describe the pre-change audit and should not be read as the status of the corrected files.
