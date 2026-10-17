PYTHON ?= .venv/bin/python

.PHONY: rhythm-fm rhythm-fm-check rhythm-uploader rhythm-uploader-check transfer-bundle rhythm-fm-investigate rhythm-dsp-tools
rhythm-dsp-tools:
	$(PYTHON) tools/build_dsp_analysis.py

rhythm-fm: rhythm-dsp-tools
	$(PYTHON) tools/build_rhythm_fm.py

rhythm-fm-check: rhythm-fm
	$(PYTHON) tools/validate_rhythm_fm.py
	$(PYTHON) tools/probe_fm_bulk_transfer.py
	$(PYTHON) tools/probe_fm_fifo_jitter.py
	$(PYTHON) tools/probe_fm_delayed_transfer.py
	$(PYTHON) tools/probe_update_crc.py

rhythm-uploader: rhythm-fm-check
	$(PYTHON) tools/prepare_rhythm_uploader.py

rhythm-uploader-check: rhythm-uploader
	$(PYTHON) tools/probe_rhythm_update_requests.py
	$(PYTHON) tools/validate_rhythm_uploader.py

transfer-bundle: rhythm-uploader-check
	$(PYTHON) tools/package_rhythm_fm.py

rhythm-fm-investigate:
	$(PYTHON) tools/investigate_fm_boot.py
