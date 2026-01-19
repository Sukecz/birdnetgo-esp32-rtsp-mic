import ast
import os
import re
import unittest


PROJECT_ROOT = os.path.dirname(os.path.dirname(__file__))
WEBUI_CPP = os.path.join(PROJECT_ROOT, "esp32_rtsp_mic_birdnetgo", "WebUI.cpp")


def _extract_html_literals(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    start = text.find("static String htmlIndex()")
    if start == -1:
        raise AssertionError("htmlIndex() not found in WebUI.cpp")

    end = text.find("return h;", start)
    if end == -1:
        raise AssertionError("htmlIndex() return not found in WebUI.cpp")

    block = text[start:end]

    # Capture all C/C++ string literals inside htmlIndex() and concat.
    literals = re.findall(r'"(?:\\.|[^"\\])*"', block)
    if not literals:
        raise AssertionError("No string literals found in htmlIndex()")

    return "".join(ast.literal_eval(lit) for lit in literals)


class TestWebUIHtml(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = _extract_html_literals(WEBUI_CPP)

    def test_has_core_sections(self):
        for marker in [
            "id='t_status'",
            "id='t_audio'",
            "id='t_perf'",
            "id='t_thermal'",
            "id='t_advanced_settings'",
            "id='t_logs'",
        ]:
            self.assertIn(marker, self.html)

    def test_has_main_controls(self):
        for marker in [
            "id='in_rate'",
            "id='in_gain'",
            "id='sel_buf'",
            "id='sel_hp'",
            "id='in_hp_cutoff'",
            "id='in_thr'",
            "id='in_chk'",
            "id='sel_tx'",
            "id='sel_cpu'",
            "id='sel_oh_enable'",
            "id='sel_oh_limit'",
            "id='btn_therm_clear'",
        ]:
            self.assertIn(marker, self.html)

    def test_has_api_endpoints(self):
        for endpoint in [
            "/api/status",
            "/api/audio_status",
            "/api/perf_status",
            "/api/thermal",
            "/api/thermal/clear",
            "/api/logs",
            "/api/action/",
            "/api/set?key=",
        ]:
            self.assertIn(endpoint, self.html)

    def test_has_language_table(self):
        self.assertIn("const T={en:{", self.html)
        self.assertIn(",cs:{", self.html)
        self.assertIn("confirm_reboot", self.html)
        self.assertIn("confirm_reset", self.html)

    def test_has_page_title(self):
        self.assertIn("<title>ESP32 RTSP Mic for BirdNET-Go</title>", self.html)


if __name__ == "__main__":
    unittest.main()
