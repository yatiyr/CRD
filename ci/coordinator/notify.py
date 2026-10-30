"""Notifications of the Cerid orchestrator (ADR-0135 §16): failure, recovery, budget, flaky quarantine and its
expiry, first-bad-commit results.

Two channel kinds, both outbound only, nothing exposed: email through the operator's own provider (SMTP with
STARTTLS or implicit TLS; the password is a Secret Manager secret) and a self-hosted ntfy server (reachable on the
tailnet; an optional access token is a secret). Every notification has a key and is sent at most once (a
create-only record in the store), so a restarted coordinator or two ticks never repeat one. A channel that fails is
logged and never fails a build.
"""
from __future__ import annotations

from email.message import EmailMessage
import hashlib
import smtplib
import ssl
import urllib.request

EVENTS = ('failure', 'recovery', 'budget', 'flaky', 'quarantine_expired', 'bisect')


def email(channel, secrets, title, body, smtp=smtplib.SMTP, smtp_ssl=smtplib.SMTP_SSL):
    message = EmailMessage()
    message['Subject'] = '[Cerid CI] %s' % title
    message['From'] = channel['from']
    message['To'] = ', '.join(channel['to'])
    message.set_content(body)
    port = int(channel.get('port', 587))
    context = ssl.create_default_context()
    if channel.get('tls', 'starttls') == 'implicit':
        server = smtp_ssl(channel['host'], port, context=context, timeout=30)
    else:
        server = smtp(channel['host'], port, timeout=30)
        server.starttls(context=context)
    try:
        if channel.get('username'):
            server.login(channel['username'], secrets.value(channel['password_secret']))
        server.send_message(message)
    finally:
        server.quit()


def ntfy(channel, secrets, title, body, priority='default', opener=urllib.request.urlopen):
    headers = {'Title': title.encode('ascii', 'replace').decode('ascii'), 'Priority': priority,
               'Tags': 'cerid-ci'}
    if channel.get('token_secret'):
        headers['Authorization'] = 'Bearer %s' % secrets.value(channel['token_secret'])
    request = urllib.request.Request(channel['url'], data=body.encode('utf-8'), method='POST', headers=headers)
    with opener(request, timeout=30) as response:
        response.read()


class Notifier:
    def __init__(self, config, secrets, store_create, log, senders=None):
        self.config = config or {}
        self.secrets = secrets
        self.create = store_create
        self.log = log
        self.senders = senders or {'email': email, 'ntfy': ntfy}

    def wanted(self, event, branch=None):
        if event not in self.config.get('events', list(EVENTS)):
            return False
        branches = self.config.get('branches', ['main'])
        return branch is None or branch in branches or '*' in branches

    def __call__(self, event, title, body, key, branch=None):
        """Send once per key to every configured channel; returns the channels that accepted it."""
        if not self.config.get('channels') or not self.wanted(event, branch):
            return []
        digest = hashlib.sha256(key.encode('utf-8')).hexdigest()[:40]
        if not self.create('notified/%s.json' % digest, {'event': event, 'title': title, 'key': key}):
            return []
        sent = []
        for channel in self.config['channels']:
            try:
                if channel['kind'] == 'ntfy':
                    self.senders['ntfy'](channel, self.secrets, title, body,
                                         priority='high' if event in ('failure', 'budget') else 'default')
                else:
                    self.senders[channel['kind']](channel, self.secrets, title, body)
                sent.append(channel['kind'])
            except Exception as error:  # a notification is courtesy: never fail the caller
                self.log('notify_failed', level='warning', channel=channel.get('kind'), event=event,
                         error=str(error)[:300])
        self.log('notified', event=event, title=title, channels=sent)
        return sent
