#!/usr/bin/env python3
"""Creates the secrets for the terminal's admin login. Run it on your own computer.

    python -m pip install werkzeug pyotp qrcode
    python tools/make_admin_secrets.py

It prints three values to paste into Railway, and a QR code for an authenticator
app. Nothing is written to disk or sent anywhere. Run it in a normal terminal
window: anything it prints inside a shared or recorded session is exposed.
"""

import getpass
import secrets
import string
import sys

import pyotp
import qrcode
from werkzeug.security import generate_password_hash

MIN_PASSWORD_LENGTH = 16
GENERATED_PASSWORD_LENGTH = 24
ISSUER = 'Mira'
ACCOUNT = 'admin'


def make_secret_key():
    return secrets.token_hex(32)


def make_password():
    alphabet = string.ascii_letters + string.digits
    return ''.join(secrets.choice(alphabet) for _ in range(GENERATED_PASSWORD_LENGTH))


def make_password_hash(password):
    return generate_password_hash(password, method='scrypt')


def make_totp_secret():
    return pyotp.random_base32()


def totp_uri(totp_secret):
    return pyotp.TOTP(totp_secret).provisioning_uri(name=ACCOUNT, issuer_name=ISSUER)


def code_is_valid(totp_secret, code):
    return pyotp.TOTP(totp_secret).verify(code.strip().replace(' ', ''), valid_window=1)


def ask_password():
    print('Choose the admin password.')
    print('Press Enter to have one generated, which is the safest choice.')
    while True:
        password = getpass.getpass('Password (hidden as you type): ')
        if not password:
            password = make_password()
            print()
            print('Your generated password. Save it in your password manager now:')
            print()
            print('    ' + password)
            print()
            input('Press Enter once it is saved...')
            return password
        if len(password) < MIN_PASSWORD_LENGTH:
            print(f'Use at least {MIN_PASSWORD_LENGTH} characters.')
            continue
        if getpass.getpass('Type it again: ') != password:
            print('Those did not match.')
            continue
        return password


def show_qr(uri):
    qr = qrcode.QRCode(border=2)
    qr.add_data(uri)
    qr.make(fit=True)
    qr.print_ascii(invert=True)


def confirm_app(totp_secret):
    print()
    print('Microsoft Authenticator: tap +, then "Other account", then scan this code.')
    print()
    show_qr(totp_uri(totp_secret))
    print()
    print('If the code will not scan, choose "Enter code manually" and use:')
    print()
    print(f'    Account name:  {ISSUER}')
    print(f'    Secret key:    {totp_secret}')
    print()
    while True:
        code = input('Type the 6-digit code the app shows now: ')
        if code_is_valid(totp_secret, code):
            print('The code matches.')
            return
        print('That code did not match. Wait for the next one and try again.')


def main():
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        sys.exit('Run this in a normal terminal window, not through another program.')

    password_hash = make_password_hash(ask_password())
    totp_secret = make_totp_secret()
    confirm_app(totp_secret)

    print()
    print('Add these three variables in Railway, then close this window:')
    print()
    print(f'SECRET_KEY={make_secret_key()}')
    print(f'ADMIN_PASSWORD_HASH={password_hash}')
    print(f'ADMIN_TOTP_SECRET={totp_secret}')
    print()


if __name__ == '__main__':
    try:
        main()
    except (KeyboardInterrupt, EOFError):
        sys.exit('\nStopped. Nothing was saved.')
