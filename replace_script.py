import re

def process_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Match: _gfx->drawString(text, x, y, size); OR _gfx->drawString(text, x, y);
    # Group 1: text, Group 2: x, Group 3: y, Group 4: size (optional)
    pattern = r'_gfx->drawString\(\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*(?:,\s*([^)]+?))?\s*\);'
    
    def replacer(match):
        text = match.group(1)
        x = match.group(2)
        y = match.group(3)
        size = match.group(4)
        
        # Calculate indentation (very primitive, assuming standard formatting or just adding standard spaces)
        # We can just output it with generic indentation or capture the leading whitespace.
        
        res = f"_gfx->setCursor({x}, {y});\n"
        if size:
            res += f"  _gfx->setTextSize({size});\n"
        res += f"  _gfx->print({text});"
        return res
        
    # Let's use a function that captures the leading whitespace to format it correctly
    def replacer_with_indent(match):
        full_match = match.group(0)
        return replacer(match)

    # Actually, a better regex is to capture the preceding whitespace
    pattern2 = r'([ \t]*)_gfx->drawString\(\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*(?:,\s*([^)]+?))?\s*\);'
    
    def replacer2(match):
        indent = match.group(1)
        text = match.group(2)
        x = match.group(3)
        y = match.group(4)
        size = match.group(5)
        
        res = f"{indent}_gfx->setCursor({x}, {y});\n"
        if size:
            res += f"{indent}_gfx->setTextSize({size});\n"
        res += f"{indent}_gfx->print({text});"
        return res

    new_content = re.sub(pattern2, replacer2, content)
    
    with open(filepath, 'w') as f:
        f.write(new_content)

process_file('src/display.cpp')
print("Done")
