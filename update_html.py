import re

with open('src/web_server_mgr.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

match = re.search(r'R"rawhtml\((.*?)\)rawhtml";', content, re.DOTALL)
if match:
    with open('data/index.html', 'w', encoding='utf-8') as f:
        f.write(match.group(1).strip())
    print("Successfully updated data/index.html")
else:
    print("Could not find rawhtml block")
