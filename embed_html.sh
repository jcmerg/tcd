#!/bin/bash
# Convert monitor.html to a C const char array for embedding
INPUT="monitor.html"
OUTPUT="monitor_html.cpp"

echo "// Auto-generated from monitor.html — do not edit" > "$OUTPUT"
echo "#include <cstddef>" >> "$OUTPUT"
echo "" >> "$OUTPUT"
echo -n 'extern const char monitor_html[] = R"RAWHTML(' >> "$OUTPUT"
cat "$INPUT" >> "$OUTPUT"
echo ')RAWHTML";' >> "$OUTPUT"
echo "" >> "$OUTPUT"
echo "extern const unsigned int monitor_html_len = sizeof(monitor_html) - 1;" >> "$OUTPUT"
echo "Generated $OUTPUT from $INPUT"
