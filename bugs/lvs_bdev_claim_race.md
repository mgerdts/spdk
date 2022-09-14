When an lvstore is being loaded it does roughly the following:

1. Open the bdev containing the blobstore
2. Read metadata from the blobstore
3. Claim the bdev opened in 1
4. Carry on like the data read in step 2 is trustworthy.

Before step 3 completes, someone else may have been writing to the blobstore
even as the lvstore is trusting the metadata it is reading.

The following should happen:

1. lvstore should claim the blobstore device before it starts reading.
2. The bdev claim mechanism should be robust enough that it fails if there are
   any open writers and the claim blocks any new writers.
