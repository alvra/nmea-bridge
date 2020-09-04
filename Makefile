all: js css


js: js_fragments.cpp

css: css_fragments.cpp


js_fragments.cpp: static/log_script.js make_js_fragments.py
	./make_js_fragments.py


css_fragments.cpp: static/style.css make_css_fragments.py
	./make_css_fragments.py
