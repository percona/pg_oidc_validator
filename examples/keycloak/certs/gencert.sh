#!/bin/bash
# Command to generate the files in the certs folder.
# Since they have a 10 year lifetime, running this isn't necessary for a long time.
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out crt.pem -sha256 -days 3650 -nodes \
  -subj "/C=XX/ST=StateName/L=CityName/O=CompanyName/OU=CompanySectionName/CN=keycloak" \
  -addext "subjectAltName=DNS:keycloak,DNS:localhost,IP:127.0.0.1"
