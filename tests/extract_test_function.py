"""Extract a production function, retaining its source location for coverage."""
import json
import pathlib
import sys


def code_only(text):
    """Mask comments and quoted literals without moving source offsets."""
    result = list(text)
    i = 0
    while i < len(text):
        start = i
        if text.startswith('//', i):
            end = text.find('\n', i + 2)
            i = len(text) if end < 0 else end
        elif text.startswith('/*', i):
            end = text.find('*/', i + 2)
            if end < 0:
                raise ValueError('unterminated comment')
            i = end + 2
        elif text[i] in ('"', "'"):
            quote = text[i]
            i += 1
            while i < len(text):
                if text[i] == '\\':
                    i += 2
                elif text[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
        else:
            i += 1
            continue
        for j in range(start, min(i, len(text))):
            if text[j] != '\n':
                result[j] = ' '
    return ''.join(result)


path = pathlib.Path(sys.argv[1]).resolve()
text = path.read_text()
masked = code_only(text)
start = masked.index(sys.argv[2])
brace = masked.index('{', start)
depth = 1
end = brace + 1
while depth and end < len(masked):
    depth += (masked[end] == '{') - (masked[end] == '}')
    end += 1
if depth:
    raise ValueError('unterminated function: ' + sys.argv[2])
print('#line %d %s' % (text.count('\n', 0, start) + 1, json.dumps(str(path))))
print(text[start:end])
