# Shared production executables for portable, signing and symbol packaging.
$PulseReleaseExecutables = @('pulse.exe', 'Pulse.Index.exe',
    'Pulse.Preview.exe', 'pulse_shell.exe', 'pulse_integration.exe', 'pulse_elevated.exe')
$PulseReleaseRuntimeDlls = @('lumatext.dll', 'pdfium.dll')
$PulseReleasePayload = $PulseReleaseExecutables + $PulseReleaseRuntimeDlls
# The content enhancement pack (content search, duplicate finding, document
# text). Built, signed and symbol-archived like the payload, but never in the
# installer or ZIP: the pulse_content_pack target writes LZMS release assets to
# <build>/content-pack together with the catalog compiled into pulse.exe, and
# pulse.exe downloads them from the same GitHub release on first use.
$PulseContentPackExecutables = @('Pulse.Content.exe', 'Pulse.Document.exe')
# The AI enhancement pack (the MCP server AI apps connect to), released the same
# way: the pulse_ai_pack target writes <build>/ai-pack, key "ai".
$PulseAiPackExecutables = @('Pulse.AI.exe')
$PulsePackExecutables = $PulseContentPackExecutables + $PulseAiPackExecutables
$PulseSignedExecutables = $PulsePackExecutables + $PulseReleaseExecutables