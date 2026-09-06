'use strict';
'require view';
'require poll';
'require dom';
'require ui';
'require rpc as luciRpc';
'require qmodem-voip.rpc as rpc';
'require qmodem-voip.reducer as reducer';
'require qmodem-voip.media as media';
'require qmodem-voip.surface as surface';

const POLL_SECONDS = 1, WORKLET_URL = L.resource('qmodem-voip/audio-worklet.js');
const STATE_LABELS = {
	disabled: _('Disabled'), idle: _('Ready'), outgoing_setup: _('Outgoing'), incoming_ringing: _('Incoming call'), early_media: _('Early media'), active: _('Active'), terminating: _('Ending call'), recovering: _('Recovering'), fault: _('Fault')
};
const STATE_STYLES = {
	outgoing_setup: 'warning', incoming_ringing: 'warning', early_media: 'warning', active: 'success', terminating: 'warning', recovering: 'warning', fault: 'error'
};

document.head.appendChild(E('link', { rel: 'stylesheet', type: 'text/css', href: L.resource('qmodem-voip/qmodem-voip.css') }));

return view.extend({
	load() {
		this.state = reducer.initialState();
		this.rpc = rpc;
		this.media = media.createMediaClient({
			issueToken: rpc.issueMediaToken,
			onDisconnect: () => {
				this.mediaAttached = false;
				if (this.root)
					this.updateView();
			}
		});
		return Promise.all([
			L.resolveDefault(rpc.capabilities(), {}),
			L.resolveDefault(rpc.status(), {})
		]).then((results) => {
			this.capabilityPending = !results[0] || Object.keys(results[0]).length === 0;
			this.capabilityRetryAt = 0;
			this.dispatch({ type: 'CAPABILITIES', value: results[0] });
			this.dispatch({ type: 'SNAPSHOT', value: results[1] });
			return results;
		});
	},

	dispatch(action) {
		this.state = reducer.reduce(this.state, action);
		if (this.root)
			this.updateView();
	},

	async refresh() {
		if (this.mediaConnecting)
			return null;
		if (this.capabilityPending && Date.now() >= this.capabilityRetryAt) {
			this.capabilityRetryAt = Date.now() + 2000;
			const capabilities = await L.resolveDefault(rpc.capabilities(), null);
			if (capabilities && Object.keys(capabilities).length !== 0) {
				this.capabilityPending = false;
				this.dispatch({ type: 'CAPABILITIES', value: capabilities });
			}
		}
		const request = L.resolveDefault(rpc.status(), null);
		this.statusRequest = request;
		const snapshot = await request;
		if (this.statusRequest === request)
			this.statusRequest = null;
		if (snapshot) {
			this.dispatch({ type: 'SNAPSHOT', value: snapshot });
			await this.syncMedia(snapshot);
		}
		return snapshot;
	},

	showError(error) {
		this.dispatch({ type: 'ERROR', value: error });
		if (error)
			ui.addNotification(_('Call operation failed'), E('p', {}, [ reducer.errorMessage(error) ]), 'error');
	},

	async run(action, payload, after) {
		this.dispatch({ type: 'CLEAR_ERROR' });
		try {
			const response = await rpc[action](...(Array.isArray(payload) ? payload : []));
			if (response?.status === 'error') {
				if (action === 'setSipCredentials')
					this.dispatch({ type: 'CREDENTIAL_RESULT', value: response });
				if (action === 'issueMediaToken')
					this.dispatch({ type: 'MEDIA_RESULT', value: response });
				this.showError(response);
				return null;
			}
			if (response && action === 'setSipCredentials')
				this.dispatch({ type: 'CREDENTIAL_RESULT', value: response });
			else if (response && action === 'issueMediaToken')
				this.dispatch({ type: 'MEDIA_RESULT', value: response });
			else if (response)
				this.dispatch({ type: 'SNAPSHOT', value: response });
			if (after)
				after(response);
			return response;
		}
		catch (error) {
			this.showError({ error: error.code || 'unknown', message: error.message });
			return null;
		}
	},

	async saveCredentials(event) {
		event.preventDefault();
		const password = this.refs.sipPassword.value;
		if (!this.refs.sipUser.value.trim() || !password)
			return;
		await this.run('setSipCredentials', [ this.refs.sipUser.value.trim(), password ], () => { this.refs.sipPassword.value = ''; });
		this.refs.sipPassword.value = '';
	},

	async originate(event) {
		event.preventDefault();
		const number = this.refs.dial.value.trim();
		if (!number)
			return;
		try {
			/* The audio action and form submit are separate DOM events.  A user can
			 * submit while getUserMedia() is still resolving; serialize them so the
			 * idle refresh in startMedia() cannot release the call's stream. */
			if (this.mediaStartRequest)
				await this.mediaStartRequest;
			await this.requestMediaStream();
		}
		catch (error) {
			this.handleMediaError(error);
			return;
		}
		const response = await this.run('originate', [ 'browser', number ]);
		if (!response) {
			this.releasePendingMedia();
			return;
		}
		this.refs.dial.value = '';
		await this.syncMedia(response);
	},

	async answer() {
		try {
			await this.requestMediaStream();
		}
		catch (error) {
			this.handleMediaError(error);
			return null;
		}
		const response = await this.run('answer', [ 'browser' ]);
		if (!response)
			this.releasePendingMedia();
		else
			await this.syncMedia(response);
		return response;
	},

	terminate(action) {
		this.refs.dial.value = '';
		this.closeMedia();
		return this.run(action, [ 'browser' ]);
	},

	toggleMute() {
		this.muted = !this.muted;
		this.media.setMuted(this.muted);
		this.refs.mute.setAttribute('aria-pressed', this.muted ? 'true' : 'false');
		this.refs.mute.textContent = this.muted ? _('Unmute') : _('Mute');
	},

	async requestMediaStream() {
		if (this.media.hasAudio())
			return null;
		if (this.pendingMediaStream)
			return this.pendingMediaStream;
		if (this.mediaStreamRequest)
			return this.mediaStreamRequest;
		if (this.state.mediaStatus !== 'ready' || !navigator.mediaDevices?.getUserMedia)
			throw Object.assign(new Error('browser audio is unavailable'), { code: 'unsupported' });
		this.mediaStreamRequest = navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, sampleRate: 48000 } });
		try {
			this.pendingMediaStream = await this.mediaStreamRequest;
			this.dispatch({ type: 'MEDIA_PERMISSION', value: 'granted' });
			return this.pendingMediaStream;
		}
		finally {
			this.mediaStreamRequest = null;
		}
	},

	releasePendingMedia() {
		if (this.pendingMediaStream)
			this.pendingMediaStream.getTracks().forEach((track) => track.stop());
		this.pendingMediaStream = null;
	},

	closeMedia() {
		this.releasePendingMedia();
		this.media.close();
		this.mediaAttached = false;
	},

	handleMediaError(error) {
		this.releasePendingMedia();
		this.media.close();
		this.mediaAttached = false;
		this.dispatch({ type: 'MEDIA_PERMISSION', value: error.name === 'NotAllowedError' ? 'denied' : 'error' });
		this.showError({ error: error.code || 'unsupported', message: error.message });
	},

	async syncMedia(snapshot) {
		if ([ 'idle', 'disabled', 'fault' ].indexOf(snapshot?.state) !== -1) {
			this.closeMedia();
			return;
		}
		if ([ 'outgoing_setup', 'early_media', 'active' ].indexOf(snapshot?.state) === -1 ||
			(!this.pendingMediaStream && !this.media.hasAudio()) || this.media.isConnected() ||
			this.mediaConnecting || !this.state.mediaUrl)
			return;
		this.mediaConnecting = true;
		const pollingPaused = poll.active() ? poll.stop() : false;
		try {
			/* Safari may reject a same-origin WebSocket when a status RPC is still
			 * in flight.  Drain it and keep polling stopped through the upgrade. */
			if (this.statusRequest)
				await this.statusRequest;
			if (!this.media.hasAudio())
				await this.media.attachAudio(this.pendingMediaStream, WORKLET_URL);
			/* Build the playback node before opening the socket.  The modem can
			 * produce PCM immediately after the WebSocket upgrade; attaching first
			 * prevents those initial frames from being dropped before onmessage is
			 * installed. */
			await this.media.connect({ media: 'ready', url: this.state.mediaUrl, sessionId: luciRpc.getSessionID(), callRevision: snapshot.revision, httpsOrigin: location.origin });
			this.pendingMediaStream = null;
			this.mediaAttached = true;
			this.dispatch({ type: 'MEDIA_PERMISSION', value: 'granted' });
		}
		catch (error) {
			this.mediaAttached = false;
			if (!this.media.hasAudio())
				this.handleMediaError(error);
		}
		finally {
			this.mediaConnecting = false;
			if (pollingPaused)
				poll.start();
		}
	},

	startMedia() {
		if (this.state.mediaStatus !== 'ready')
			return;
		if (this.mediaStartRequest)
			return this.mediaStartRequest;
		this.mediaStartRequest = (async () => {
			try {
				await this.requestMediaStream();
				/* Permission prompts can outlast a call-state transition.  Refresh
				 * before minting the token so the WebSocket is opened against the
				 * current call revision as soon as audio permission is granted. */
				const snapshot = await this.refresh();
				if (snapshot)
					await this.syncMedia(snapshot);
			}
			catch (error) {
				this.handleMediaError(error);
			}
			finally {
				this.mediaStartRequest = null;
			}
		})();
		return this.mediaStartRequest;
	},

	handleEvent(event) {
		if (event?.event)
			this.dispatch({ type: 'EVENT', value: event });
	},

	updateTimer() {
		if (!this.timerStartedAt || !this.refs.timer)
			return;
		const seconds = Math.max(0, Math.floor((Date.now() - this.timerStartedAt) / 1000));
		this.refs.timer.textContent = `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`;
	},

	updateView() {
		const model = reducer.viewModel(this.state);
		const snapshot = this.state.snapshot || { state: 'disabled', enabled: false };
		const statusText = STATE_LABELS[model.state] || _('Unknown state');
		this.refs.support.textContent = model.capabilityPending ? _('Waiting') : (model.supported ? _('Supported') : _('Unsupported'));
		this.refs.support.className = `label ${model.supported ? 'success' : 'warning'}`;
		this.refs.capability.textContent = model.capabilityPending ? _('Waiting for modem discovery and the read-only safety probe.') : (model.supported ? _('RM520N-GL passed the safety probe.') : _('The read-only hardware probe did not pass. Call controls stay unavailable.'));
		this.refs.enable.checked = model.enabled;
		this.refs.enable.disabled = !model.supported || !model.stable;
		this.refs.enableLabel.textContent = model.enabled ? _('Enabled') : _('Disabled');
		this.refs.sipStatus.textContent = this.state.credentialStatus === 'not_ready' ? _('SIP credential rotation is not ready in this backend.') : (model.registration === 'configured' ? _('Credentials configured. Registration is not reported by v1 status.') : _('SIP account is not configured. Registration is not reported by v1 status.'));
		this.refs.callStatus.textContent = statusText;
		this.refs.callStatus.className = `label ${STATE_STYLES[model.state] || ''}`.trim();
		this.refs.callDetail.textContent = model.disabledReason || (snapshot.caller_id_withheld ? _('Caller ID withheld.') : (snapshot.number_present ? _('Destination accepted by the modem.') : _('No active call.')));
		this.refs.dial.disabled = !model.canOriginate;
		this.refs.dialForm.querySelector('button').disabled = !model.canOriginate;
		this.refs.hangup.disabled = !model.canHangup;
		this.refs.mute.disabled = !model.canMute;
		this.refs.mediaStatus.textContent = this.mediaAttached ? _('Browser audio is connected.') : (this.state.mediaStatus === 'ready' ? _('Browser audio connects while the call is being established.') : _('Media backend is not ready. Browser audio remains inactive.'));
		this.refs.mediaAction.disabled = this.state.mediaStatus !== 'ready';
		this.refs.permission.textContent = this.state.mediaPermission === 'denied' ? _('Microphone permission was denied. Allow it in the browser and try again.') : (this.state.mediaPermission === 'granted' ? _('Microphone permission is active.') : _('No microphone permission has been requested.'));
		this.refs.error.textContent = model.errorText;
		this.refs.error.hidden = !model.errorText;
		surface.setIncomingModal(this, model.canAnswer);
		if (model.callTimerVisible && !this.timerStartedAt)
			this.timerStartedAt = Date.now();
		if (!model.callTimerVisible)
			this.timerStartedAt = null;
		this.updateTimer();
		this.refs.live.textContent = this.state.lastStatusKey !== this.lastRenderedStatus ? statusText : '';
		this.lastRenderedStatus = this.state.lastStatusKey;
	},

	render() {
		surface.build(this);
		this.updateView();
		this.timerInterval = setInterval(() => this.updateTimer(), 1000);
		this.pollFn = () => this.refresh();
		poll.add(this.pollFn, POLL_SECONDS);
		return this.root;
	},

	remove() {
		if (this.pollFn)
			poll.remove(this.pollFn);
		this.closeMedia();
		if (this.timerInterval)
			clearInterval(this.timerInterval);
		surface.setIncomingModal(this, false);
	}
});
